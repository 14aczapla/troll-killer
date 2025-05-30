/* ssh lab-net-25 true
mpirun -H lab-net-25:4,lab-net-26:5 main*/

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

/* boolean */
#define TRUE 1
#define FALSE 0
#define ROOT 0

#define MIN 1
#define MAX 10

/* Stany procesu */
typedef enum {
    IDLE,
    REQUESTING,
    IN_CITY,
    WAITING
} state_t;

/* typ pakietu */
typedef struct {
    int src;        // ID procesu źródłowego
    int lamport;    // Zegar Lamporta
    int city;       // Miasto, o które się ubiegamy
    int type;       // Typ wiadomości: REQ lub ACK
} packet_t;

/* Element kolejki żądań */
typedef struct {
    int src;
    int lamport;
    int city;
} request_t;

/*pakiet cooldown
lamport 
miasto
cooldown*/

#define NITEMS 4
#define MAX_QUEUE_SIZE 100
#define MAX_CITIES 4

/* Typy wiadomości */
#define REQ 1
#define ACK 2

MPI_Datatype MPI_PAKIET_T;
int M = MAX_CITIES; // Liczba miast
int COOLDOWN_TIME[MAX_CITIES] = {0}; // Globalna tablica czasów odnowienia miast
int lamport_clock = 0;
state_t state = IDLE;
int my_city = -1;
int ack_count = 0;
int N; // Liczba procesów
int rank; // Rank procesu
int my_request_lamport = -1;

// Kolejka żądań dla każdego miasta
request_t city_queues[MAX_CITIES][MAX_QUEUE_SIZE];
int queue_sizes[MAX_CITIES] = {0};

void inicjuj_typ_pakietu() {
    const int nitems = NITEMS; 
    int blocklengths[NITEMS] = {1,1,1,1};
    MPI_Datatype typy[NITEMS] = {MPI_INT, MPI_INT, MPI_INT, MPI_INT};

    MPI_Aint offsets[NITEMS]; 
    offsets[0] = offsetof(packet_t, src);
    offsets[1] = offsetof(packet_t, lamport);
    offsets[2] = offsetof(packet_t, city);
    offsets[3] = offsetof(packet_t, type);

    MPI_Type_create_struct(nitems, blocklengths, offsets, typy, &MPI_PAKIET_T);
    MPI_Type_commit(&MPI_PAKIET_T);
}

int przydzial_miasta(int id, int clock) {
    if(clock == 0){
        return rand() % M;
    } else {
        return (id + clock) % M;
    }
}

void wyslij_do_wszystkich(packet_t *pkt, int tag) {
    for (int dest = 0; dest < N; dest++) {
        if (dest != rank) {
            MPI_Send(pkt, 1, MPI_PAKIET_T, dest, tag, MPI_COMM_WORLD);
        }
    }
}

bool dodaj_do_kolejki(int src, int lamport, int city) {
    if (queue_sizes[city] >= MAX_QUEUE_SIZE) {
        printf("[%d] Kolejka dla miasta %d pełna!\n", rank, city);
        return FALSE;
    }
    
    // Sprawdź czy żądanie już istnieje w kolejce
    for (int i = 0; i < queue_sizes[city]; i++) {
    if (city_queues[city][i].src == src &&
        city_queues[city][i].lamport == lamport) {
        return FALSE;  // już mamy to żądanie
    }
}

    
    city_queues[city][queue_sizes[city]].src = src;
    city_queues[city][queue_sizes[city]].lamport = lamport;
    city_queues[city][queue_sizes[city]].city = city;
    queue_sizes[city]++;
    return TRUE;
}

void usun_z_kolejki(int src, int city) {
    int found = 0;
    for (int i = 0; i < queue_sizes[city]; i++) {
        if (city_queues[city][i].src == src) {
            found = 1;
        }
        if (found && i < queue_sizes[city] - 1) {
            city_queues[city][i] = city_queues[city][i+1];
        }
    }
    if (found) {
        queue_sizes[city]--;
    }
}

int porownaj_zadania(request_t a, request_t b) {
    if (a.lamport != b.lamport) {
        return a.lamport - b.lamport;
    }

    return a.src - b.src;
}

void sortuj_kolejke(int city) {
    for (int i = 0; i < queue_sizes[city]-1; i++) {
        for (int j = 0; j < queue_sizes[city]-i-1; j++) {
            if (porownaj_zadania(city_queues[city][j], city_queues[city][j+1]) > 0) {
                request_t temp = city_queues[city][j];
                city_queues[city][j] = city_queues[city][j+1];
                city_queues[city][j+1] = temp;
            }
        }
    }
}

void obsluz_kolejke(int city) {
    sortuj_kolejke(city);
    
    printf("[%d] Wysyłam ACK dla miasta %d (z kolejki)\n", rank, city);
    for (int i = 0; i < queue_sizes[city]; i++) {
        request_t req = city_queues[city][i];
            packet_t ack_pkt = {rank, lamport_clock, city, ACK};
            MPI_Send(&ack_pkt, 1, MPI_PAKIET_T, req.src, ACK, MPI_COMM_WORLD);
            usun_z_kolejki(req.src, city);
            i--; // Ponieważ usunęliśmy element
    }
}

void obsluz_ack(packet_t *pkt) {
    lamport_clock = (pkt->lamport > lamport_clock) ? pkt->lamport : lamport_clock;
    ack_count++;
    
    if (ack_count == N-1) {     
        if (COOLDOWN_TIME[my_city] == 0) { //i miasto wolne
            int stay_time = rand() % (MAX - MIN + 1) + MIN;
            printf("\033[1;33m[%d] Wchodzę do miasta %d (Lamport: %d)\033[0m\n", rank, my_city, lamport_clock);
            
            state = IN_CITY;
            int start_clock = lamport_clock;
            
            while (lamport_clock - start_clock < stay_time) {
                // Sprawdzanie wiadomości podczas pobytu w mieście
                int flag;
                MPI_Status status;
                MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
                
                if (flag) {
                    packet_t pkt;
                    MPI_Recv(&pkt, 1, MPI_PAKIET_T, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
                    //lamport_clock = (pkt.lamport > lamport_clock) ? pkt.lamport : lamport_clock;
                    //lamport_clock++;
                    
                    if (pkt.type == REQ) {
                        // Obsługa REQ podczas pobytu w mieście
                        int city = pkt.city;
                        if (city != my_city) {
                            packet_t ack_pkt = {rank, lamport_clock, city, ACK};
                            MPI_Send(&ack_pkt, 1, MPI_PAKIET_T, pkt.src, ACK, MPI_COMM_WORLD);
                            printf("\033[1;31m[%d] Wysyłam ACK do %d dla miasta %d (Lamport: %d) podczas pobytu w mieście %d\033[0m\n", 
                                   rank, pkt.src, city, lamport_clock, my_city);
                        } else {
                            dodaj_do_kolejki(pkt.src, pkt.lamport, city); //jezei w niej nie jest
                            //printf("[%d] Odkładam żądanie od %d dla miasta %d do kolejki (Rozmiar: %d) podczas pobytu w mieście\033[0m\n", 
                                   //rank, pkt.src, city, queue_sizes[city]);
                        }
                    }
                }
                
                lamport_clock += 1;
            }
            
            printf("\033[1;32m[%d] Opuszczam miasto %d po %d sekundach\033[0m\n", rank, my_city, stay_time);
            srand(time(0));
            // Ustawienie czasu odnowienia dla miasta
            COOLDOWN_TIME[my_city] = rand() % 5 + 5;
            //send cooldown do wszystkich
            //printf("cooldown time: %d\n", COOLDOWN_TIME[my_city]);
            state = IDLE;
            //my_city = -1;
            ack_count = 0;
            
            obsluz_kolejke(my_city); //wyslanie ack do procesow w kolejce od miasta
            my_city = -1;
        } else {
            printf("[%d] Miasto %d niedostępne (cooldown: %d), czekam...\n", rank, my_city, COOLDOWN_TIME[my_city]);
            state = WAITING;
        }
    }
}

void obsluz_req(packet_t *pkt) {
    int req_lamport_clock = my_request_lamport;
    lamport_clock = (pkt->lamport > lamport_clock) ? pkt->lamport : lamport_clock;
    lamport_clock++;
    
    int city = pkt->city;
    
    // Sprawdź czy możemy wysłać ACK
    if (state == IDLE || 
    (state == REQUESTING && (city != my_city)) ||
    (state == IN_CITY && city != my_city) ||
    (state == REQUESTING && city == my_city &&
        (pkt->lamport < req_lamport_clock ||
        (pkt->lamport == req_lamport_clock && pkt->src < rank)))) {

        packet_t ack_pkt = {rank, lamport_clock, city, ACK};
        MPI_Send(&ack_pkt, 1, MPI_PAKIET_T, pkt->src, ACK, MPI_COMM_WORLD);
        printf("\033[1;31m[%d] Wysyłam ACK do %d dla miasta %d (Lamport: %d)\033[0m\n", rank, pkt->src, city, lamport_clock);
    } else {
         //jezeli w niej nie jest
        if(dodaj_do_kolejki(pkt->src, pkt->lamport, city)) {
        //printf("[%d]Odkładam żądanie od %d dla miasta %d do kolejki (Rozmiar: %d)\n", 
                //rank, pkt->src, city, queue_sizes[city]);
        }
    }
}

void aktualizuj_cooldown(int clock) {
    for (int city = 0; city < M; city++) {
        if (COOLDOWN_TIME[city] > 0) {
            COOLDOWN_TIME[city] -= 1;
            if (COOLDOWN_TIME[city] == 0) {
                printf("[%d] Miasto %d jest już dostępne (clock: %d \n", rank, city, clock);
                // Obsłuż kolejkę dla tego miasta gdy stanie się dostępne
                //obsluz_kolejke(city);
            }
        }
    }
    
    // Jeśli czekaliśmy na dostępne miasto
    // if (state == WAITING && COOLDOWN_TIME[my_city] == 0) {
    //     state = REQUESTING;
    //     lamport_clock++;
    //     packet_t req_pkt = {rank, lamport_clock, my_city, REQ};
    //     wyslij_do_wszystkich(&req_pkt, REQ);
    //     ack_count = 0;
    //     printf("[%d] Ponawiam REQ dla miasta %d (Lamport: %d)\n", rank, my_city, lamport_clock);
    // }
}

int main(int argc, char **argv) {
    MPI_Status status;
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &N);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    
    inicjuj_typ_pakietu();

    srand(time(0));
    
    // Główna pętla programu
    while (1) {
        aktualizuj_cooldown(lamport_clock);
        
        // Losowe decyzje o wejściu do miasta
        if (state == IDLE && (rand() % 3) == 0) {
            state = REQUESTING;
            my_city = przydzial_miasta(rank, lamport_clock);
            lamport_clock++;
            
            packet_t req_pkt = {rank, lamport_clock, my_city, REQ};
            wyslij_do_wszystkich(&req_pkt, REQ);
            my_request_lamport = lamport_clock;
            ack_count = 0;
            
            printf("\33[1;35m[%d] Wysyłam REQ dla miasta %d (Lamport: %d)\033[0m\n", rank, my_city, lamport_clock);
        }
        
        // Sprawdź czy są wiadomości do odebrania (tylko jeśli nie jesteśmy w mieście)
        if (state != IN_CITY) {
            int flag;
            MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
            
            if (flag) {
                packet_t pkt;
                MPI_Recv(&pkt, 1, MPI_PAKIET_T, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
                //lamport_clock = (pkt.lamport > lamport_clock) ? pkt.lamport : lamport_clock;
                //lamport_clock++;
                
                if (pkt.type == REQ) {
                    obsluz_req(&pkt);
                } else if (pkt.type == ACK) {
                    obsluz_ack(&pkt);
                } //else
                 //obsloz cooldown
            }
        }
        
        sleep(1); // Ograniczenie zużycia CPU
    }
    
    MPI_Type_free(&MPI_PAKIET_T);
    MPI_Finalize();
    return 0;
}