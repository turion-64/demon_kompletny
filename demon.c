#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <utime.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>

volatile sig_atomic_t signal_received = 0;

void sigusr1_handler(int signum) {
    if (signum == SIGUSR1) {
        signal_received = 1;
    }
}

int is_directory(const char *path) {
    struct stat statbuf;
    if (stat(path, &statbuf) != 0) {
        return 0;
    }
    return S_ISDIR(statbuf.st_mode);
}

// Funkcja zamieniająca proces w demona
void demonizacja() {
    //pierwszy fork odłączający program od terminala
    //w wyniku tego uzytkownik dostaje z powrotem shell
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) exit(EXIT_FAILURE);

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    //zgodnie ze sztuką wywoływania demonów powinno się robić drugi fork
    //by zapobiec przypadkowej reaktywacji terminala
    //(np. wskutek ponownego otwarcia tty)
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    chdir("/");

    int x;
    for (x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }
}

// funkcja kopiująca duże pliki za pomocą mmap (podpunkt b)
void copy_file_mmap(int fd_src, int fd_dst, size_t size) {
    // ftruncate rozszerza fd_dst do rozmiaru źródła
    if (ftruncate(fd_dst, size) == -1) {
        syslog(LOG_ERR, "Błąd ftruncate: %s", strerror(errno));
        return;
    }

    // mapowanie źródła
    void *src_map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd_src, 0);
    if (src_map == MAP_FAILED) {
        syslog(LOG_ERR, "Błąd mmap dla źródła: %s", strerror(errno));
        return;
    }

    // mapowanie celu
    void *dst_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dst, 0);
    if (dst_map == MAP_FAILED) {
        syslog(LOG_ERR, "Błąd mmap dla celu: %s", strerror(errno));
        munmap(src_map, size);
        return;
    }

    // faktyczny proces kopiowania
    memcpy(dst_map, src_map, size);

    // synchronizacja z dyskiem i odmapowanie
    msync(dst_map, size, MS_SYNC);
    munmap(src_map, size);
    munmap(dst_map, size);
}

// funkcja kopiująca plik posiadająca logikę wyboru metody
void copy_file(const char *src, const char *dst, off_t threshold) {
    //otwarcie źródła read-only
    int fd_src = open(src, O_RDONLY);
    if (fd_src < 0) return;

    struct stat st;
    if (fstat(fd_src, &st) != 0) {
        close(fd_src);
        return;
    }
    size_t file_size = st.st_size;

    //otwarcie celu do odczytu i zapisu, tworząc go jeśli nie istnieje
    int fd_dst = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (fd_dst < 0) {
        close(fd_src);
        return;
    }

    // jeśli plik jest większy niż threshold używany jest mmap (podpunkt b)
    if (file_size > (size_t)threshold) {
        syslog(LOG_INFO, "Kopiowanie mmap (duży plik: %ld bajtów): %s", file_size, src);
        copy_file_mmap(fd_src, fd_dst, file_size);
    } else {
        syslog(LOG_INFO, "Kopiowanie read/write (mały plik: %ld bajtów): %s", file_size, src);

        char buffer[8192];
        ssize_t bytes_read;
        
        while ((bytes_read = read(fd_src, buffer, sizeof(buffer))) > 0) {
            write(fd_dst, buffer, bytes_read);
        }
    }

    // zamknięcie plików
    close(fd_src);
    close(fd_dst);

    // kopiowanie czasu modyfikacji i dostępu
    struct utimbuf new_time;
    new_time.actime = st.st_atime;
    new_time.modtime = st.st_mtime;
    utime(dst, &new_time);
}

// czyści katalog rekurencyjnie
void remove_recursive(const char *path) {
    DIR *dir = opendir(path);
    if (dir == NULL) return;

    struct dirent *entry;
    //tak informacyjnie path_max zwykle rozwija się do 4096
    char full_path[PATH_MAX];
    struct stat st;

    //dla każdego wpisu
    while ((entry = readdir(dir)) != NULL) {
        //pomijamy wpis z d_name . i ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        //tworzymy full path
        snprintf(full_path, PATH_MAX, "%s/%s", path, entry->d_name);

        if (lstat(full_path, &st) == 0) {
            //znaleźliśmy podkatalog
            if (S_ISDIR(st.st_mode)) {
                syslog(LOG_INFO, "Schodze glebiej do podkatalogu: %s", full_path);
                remove_recursive(full_path);
            } else {
                syslog(LOG_INFO, "Usuwanie pliku: %s", full_path);
                unlink(full_path);
            }
        }
    }
    closedir(dir);

    syslog(LOG_INFO, "Usuwanie pustego katalogu: %s", path);
    rmdir(path);
}

// Główna funkcja synchronizująca
void synchronize_directories(const char *source_dir, const char *target_dir, off_t threshold, int recursive) {
    DIR *dir;
    struct dirent *entry;
    struct stat src_st, tgt_st;
    char src_path[PATH_MAX], tgt_path[PATH_MAX];

    //1. Najpierw kopiujemy ze źródła do celu
    dir = opendir(source_dir);
    if (dir == NULL) {
        syslog(LOG_ERR, "Nie mozna otworzyc katalogu zrodlowego: %s", source_dir);
        return;
    }

    //dla każdego wpisu
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        //budujemy ścieżkę źródła
        snprintf(src_path, PATH_MAX, "%s/%s", source_dir, entry->d_name);
        //budujemy ścieżkę celu
        snprintf(tgt_path, PATH_MAX, "%s/%s", target_dir, entry->d_name);

        //jeśli nie uda się pobrać informacji o pliku pomijamy go
        if (lstat(src_path, &src_st) != 0) continue;

        if (S_ISREG(src_st.st_mode)) {
            // warunki: nie ma pliku w celu (a), plik jest starszy(b)
            if (lstat(tgt_path, &tgt_st) != 0 || src_st.st_mtime > tgt_st.st_mtime) {
                copy_file(src_path, tgt_path, threshold);
            }
        }
        else if (S_ISDIR(src_st.st_mode) && recursive) {
            //napotkaliśmy katalog. Jeśli go nie ma w celu, tworzymy go
            if (lstat(tgt_path, &tgt_st) != 0) {
                mkdir(tgt_path, src_st.st_mode); 
                syslog(LOG_INFO, "Utworzono katalog: %s", tgt_path);
            }
            synchronize_directories(src_path, tgt_path, threshold, recursive);
        }
    }
    closedir(dir);

    //2. Następnie usuwamy z celu pliki, których nie ma w źródle
    //zasada działania jest niemal identyczna jak w punkcie 1. więc komentarze są zbędne
    dir = opendir(target_dir);
    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

            snprintf(src_path, PATH_MAX, "%s/%s", source_dir, entry->d_name);
            snprintf(tgt_path, PATH_MAX, "%s/%s", target_dir, entry->d_name);

            if (lstat(tgt_path, &tgt_st) != 0) continue; 

            if (lstat(src_path, &src_st) != 0 && errno == ENOENT) {
                if (S_ISDIR(tgt_st.st_mode) && recursive) {
                    syslog(LOG_INFO, "Usuwanie katalogu: %s", tgt_path);
                    remove_recursive(tgt_path);  
                } else if (S_ISREG(tgt_st.st_mode)) {
                    syslog(LOG_INFO, "Usuwanie pliku: %s", tgt_path);
                    unlink(tgt_path);
                }
            }
        }
        closedir(dir);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Użycie: %s <zrodlo> <cel> [czas_spania] [-R] [prog_mmap_w_bajtach]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (!is_directory(argv[1]) || !is_directory(argv[2])) {
        fprintf(stderr, "Błąd: Obie ścieżki muszą być istniejącymi katalogami.\n");
        return EXIT_FAILURE;
    }

    char source_path[PATH_MAX], target_path[PATH_MAX];
    if (!realpath(argv[1], source_path) || !realpath(argv[2], target_path)) {
        perror("Błąd rozwiązywania ścieżek");
        return EXIT_FAILURE;
    }

    // argumenty opcjonalne
    // domyślnie czas snu: 5 minut, brak rekurencji, próg mmap 10MB
    int sleep_time = 300; 
    int recursive = 0;
    off_t threshold = 10 * 1024 * 1024;
    
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-R") == 0) {
            recursive = 1;
        } else if (atoi(argv[i]) > 0 && i == 3) { 
            sleep_time = atoi(argv[i]);
        } else if (atol(argv[i]) > 0) {
            threshold = atol(argv[i]);
        }
    }

    demonizacja();

    // start logowania
    openlog("DEMONIARZ", LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "Demon synchronizacji uruchomiony. Źródło: %s, Cel: %s, Czas: %d s, Rekurencja: %d, Próg: %ld", 
           source_path, target_path, sleep_time, recursive, threshold);

    // rejestracja obsługi SIGUSR1
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa, NULL);

    // pętla główna
    while (1) {
        // zaczynamy od snu
        if (!signal_received) {
            syslog(LOG_INFO, "Demon usypia na %d sekund...", sleep_time);
            int time_left = sleep_time;

            while (time_left > 0 && !signal_received) {
                time_left = sleep(time_left);
            }
        }

        if (signal_received) {
            syslog(LOG_INFO, "Demon wybudzony natychmiastowo przez sygnał SIGUSR1.");
            signal_received = 0;
        } else {
            syslog(LOG_INFO, "Demon wybudzony naturalnie po upływie czasu snu.");
        }

        synchronize_directories(source_path, target_path, threshold, recursive);
    }

    closelog();
    return EXIT_SUCCESS;
}
