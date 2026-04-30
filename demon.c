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

// Obsługa sygnału SIGUSR1
void sigusr1_handler(int signum) {
    if (signum == SIGUSR1) {
        signal_received = 1;
    }
}

// Sprawdzenie, czy ścieżka jest katalogiem
int is_directory(const char *path) {
    struct stat statbuf;
    if (stat(path, &statbuf) != 0) {
        return 0;
    }
    return S_ISDIR(statbuf.st_mode);
}

// Funkcja zamieniająca proces w demona
void daemonize() {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS); // Zakończenie procesu macierzystego

    if (setsid() < 0) exit(EXIT_FAILURE); // Nowa sesja

    // Ignorowanie sygnałów terminala
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork(); // Drugi fork dla pewności (brak dostępu do terminala)
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0); // Wyzerowanie maski uprawnień
    chdir("/"); // Zmiana katalogu roboczego

    // Zamknięcie standardowych strumieni
    int x;
    for (x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }
}

// Funkcja kopiująca duże pliki za pomocą mmap (Podpunkt b)
void copy_file_mmap(int fd_src, int fd_dst, size_t size) {
    // Mapujemy plik źródłowy do pamięci RAM procesu
    void *src_map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd_src, 0);

    if (src_map == MAP_FAILED) {
        syslog(LOG_ERR, "Błąd mmap: %s", strerror(errno));
        return;
    }

    // Zapisujemy zmapowaną pamięć bezpośrednio do pliku docelowego
    ssize_t written = write(fd_dst, src_map, size);

    if (written < 0) {
        syslog(LOG_ERR, "Błąd zapisu mmap: %s", strerror(errno));
    }

    // Odmapowanie pamięci
    munmap(src_map, size);
}

// Funkcja kopiująca plik posiadająca logikę wyboru między duzymi a małymi plikami
void copy_file(const char *src, const char *dst, off_t threshold) {
    int fd_src = open(src, O_RDONLY);
    if (fd_src < 0) return;

    struct stat st;
    fstat(fd_src, &st);
    size_t file_size = st.st_size;

    int fd_dst = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (fd_dst < 0) {
        close(fd_src);
        return;
    }

    // Logika wyboru metody
    if (file_size > (size_t)threshold) {        // plik duzy
        syslog(LOG_INFO, "Kopiowanie mmap (duży plik: %ld bajtów): %s", file_size, src);
        copy_file_mmap(fd_src, fd_dst, file_size);
    } else {        // plik maly
        syslog(LOG_INFO, "Kopiowanie read/write (mały plik: %ld bajtów): %s", file_size, src);

        char buffer[8192];
        ssize_t bytes_read;
        while ((bytes_read = read(fd_src, buffer, sizeof(buffer))) > 0) {
            write(fd_dst, buffer, bytes_read);
        }
    }

    close(fd_src);
    close(fd_dst);

    // Kopiowanie daty modyfikacji
    struct utimbuf new_times;
    new_times.actime = st.st_atime;
    new_times.modtime = st.st_mtime;
    utime(dst, &new_times);
}

// czysci podkatalogi do usuniecia
void remove_recursive(const char *path) {
    DIR *dir = opendir(path);
    if (dir == NULL) return;

    struct dirent *entry;
    char full_path[PATH_MAX];
    struct stat st;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        snprintf(full_path, PATH_MAX, "%s/%s", path, entry->d_name);

        if (lstat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
		syslog(LOG_INFO, "Schodze glebiej do podkatalogu: %s", full_path);
                remove_recursive(full_path); // schodzi głębiej
            } else {
		syslog(LOG_INFO, "Usuwanie pliku: %s", full_path);
                unlink(full_path); // usuwa plik
            }
        }
    }
    closedir(dir);

    syslog(LOG_INFO, "Usuwanie pustego katalogu: %s", path);
    rmdir(path); // usuwamy teraz już pusty katalog
}

// Główna funkcja synchronizująca
void synchronize_directories(const char *source_dir, const char *target_dir, off_t threshold, int recursive) {
    DIR *dir;
    struct dirent *entry;
    struct stat src_st, tgt_st;
    char src_path[PATH_MAX], tgt_path[PATH_MAX];

    // --- ETAP 1: Kopiowanie z katalogu źródłowego do docelowego ---
    dir = opendir(source_dir);
    if (dir == NULL) return;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        snprintf(src_path, PATH_MAX, "%s/%s", source_dir, entry->d_name);
        snprintf(tgt_path, PATH_MAX, "%s/%s", target_dir, entry->d_name);

        if (lstat(src_path, &src_st) != 0) continue;

        if (S_ISREG(src_st.st_mode)) {
            // logika kopiowania plików
            if (lstat(tgt_path, &tgt_st) != 0 || src_st.st_mtime > tgt_st.st_mtime) {
                copy_file(src_path, tgt_path, threshold);
            }
        }
        else if (S_ISDIR(src_st.st_mode) && recursive) {
            // rekurencja, obsługa podkatalogów
            if (lstat(tgt_path, &tgt_st) != 0) {
                mkdir(tgt_path, src_st.st_mode); // Tworzymy katalog z uprawnieniami źródła
                syslog(LOG_INFO, "Utworzono katalog: %s", tgt_path);
            }
            // Wywołanie rekurencyjne
            synchronize_directories(src_path, tgt_path, threshold, recursive);
        }
    }
    closedir(dir);

    // --- ETAP 2: Usuwanie z katalogu docelowego plików, których nie ma w źródłowym ---
    dir = opendir(target_dir);
    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

            snprintf(src_path, PATH_MAX, "%s/%s", source_dir, entry->d_name);
            snprintf(tgt_path, PATH_MAX, "%s/%s", target_dir, entry->d_name);

            if (lstat(tgt_path, &tgt_st) != 0) continue; // jesli plik w celu zniknal sam, idź dalej


            if (lstat(src_path, &src_st) != 0 && errno == ENOENT) {
                if (S_ISDIR(tgt_st.st_mode) && recursive) {
                    syslog(LOG_INFO, "Usuwanie katalogu: %s", tgt_path);
                    remove_recursive(tgt_path);  // wyczysc katalog
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
    int recursive = 0;
    int opt;

    if (argc < 3) {
        fprintf(stderr, "Użycie: %s <katalog_źródłowy> <katalog_docelowy> [czas_spania_w_sekundach]\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Weryfikacja katalogów
    if (!is_directory(argv[1]) || !is_directory(argv[2])) {
        fprintf(stderr, "Błąd: Obie ścieżki muszą być istniejącymi katalogami.\n");
        return EXIT_FAILURE;
    }

    // Konwersja ścieżek na bezwzględne (przed uruchomieniem demona i chdir("/"))
    char source_path[PATH_MAX], target_path[PATH_MAX];
    if (!realpath(argv[1], source_path) || !realpath(argv[2], target_path)) {
        perror("Błąd rozwiązywania ścieżek");
        return EXIT_FAILURE;
    }

    int sleep_time = 300; // Domyślnie 5 minut (300 sekund)
    if (argc >= 4) {
        sleep_time = atoi(argv[3]);
        if (sleep_time <= 0) sleep_time = 300;
    }

    if (argc >= 5) {
        if (strcmp(argv[4], "-R") == 0) recursive = 1;
    }

    off_t threshold = 10 * 1024 * 1024; // Domyślnie 10MB
    if (argc >= 6) threshold = atol(argv[5]); // Jesli podano rozmiar, to zczytuje go jako nowy prog

    // Uruchomienie trybu demona
    daemonize();

    // Inicjalizacja logowania (syslog domyślnie dokleja czas do logów)
    openlog("DEMONIARZ", LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "Demon synchronizacji uruchomiony. Źródło: %s, Cel: %s, Czas snu: %d s.", source_path, target_path, sleep_time);

    // Rejestracja obsługi sygnału SIGUSR1
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa, NULL);

    // Główna pętla demona
    while (1) {
        syslog(LOG_INFO, "Demon usypia na %d sekund...", sleep_time);

        // Funkcja sleep zostanie przerwana przez pojawienie się sygnału
        sleep(sleep_time);

        if (signal_received) {
            syslog(LOG_INFO, "Demon wybudzony natychmiastowo przez sygnał SIGUSR1.");
            signal_received = 0; // Reset flagi
        } else {
            syslog(LOG_INFO, "Demon wybudzony naturalnie po upływie czasu snu.");
        }

        synchronize_directories(source_path, target_path, threshold, recursive);
    }

    closelog();
    return EXIT_SUCCESS;
}
