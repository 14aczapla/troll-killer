#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

#define TRUE 1
#define FALSE 0
#define MIN_STAY 1
#define MAX_STAY 10
#define MIN_COOLDOWN 5
#define MAX_COOLDOWN 10
#define MAX_QUEUE_SIZE 100
#define MAX_CITIES 4

typedef enum {
    IDLE,
    REQUESTING,
    IN_CITY,
    WAITING
} state_t;

typedef struct {
    int src;
    int lamport;
    int city;
    int type;
} packet_t;

typedef struct {
    int src;
    int lamport;
    int city;
} request_t;

typedef struct {
    int city;
    int cooldown;
    int lamport;
    int owner;  // Dodane: proces który ustawił cooldown
} cooldown_packet_t;

MPI_Datatype MPI_PAKIET_T;
MPI_Datatype MPI_COOLDOWN_PACKET_T;

// Global variables
int M = MAX_CITIES;
int COOLDOWN_TIME[MAX_CITIES] = {0};
int COOLDOWN_OWNER[MAX_CITIES] = {-1};  // Kto ustawił cooldown
int lamport_clock = 0;
state_t state = IDLE;
int my_city = -1;
int ack_count = 0;
int N;
int rank;
int my_request_lamport = -1;

request_t city_queues[MAX_CITIES][MAX_QUEUE_SIZE];
int queue_sizes[MAX_CITIES] = {0};

// Message types
#define REQ 1
#define ACK 2
#define COOLDOWN_UPDATE 3

void init_packet_type() {
    int blocklengths[4] = {1,1,1,1};
    MPI_Datatype typy[4] = {MPI_INT, MPI_INT, MPI_INT, MPI_INT};
    MPI_Aint offsets[4];
    
    offsets[0] = offsetof(packet_t, src);
    offsets[1] = offsetof(packet_t, lamport);
    offsets[2] = offsetof(packet_t, city);
    offsets[3] = offsetof(packet_t, type);

    MPI_Type_create_struct(4, blocklengths, offsets, typy, &MPI_PAKIET_T);
    MPI_Type_commit(&MPI_PAKIET_T);
}

void init_cooldown_packet_type() {
    int blocklengths[4] = {1, 1, 1, 1};  // Zmienione na 4
    MPI_Datatype typy[4] = {MPI_INT, MPI_INT, MPI_INT, MPI_INT};  // Zmienione na 4
    MPI_Aint offsets[4];  // Zmienione na 4
    
    offsets[0] = offsetof(cooldown_packet_t, city);
    offsets[1] = offsetof(cooldown_packet_t, cooldown);
    offsets[2] = offsetof(cooldown_packet_t, lamport);
    offsets[3] = offsetof(cooldown_packet_t, owner);  // Dodane
    
    MPI_Type_create_struct(4, blocklengths, offsets, typy, &MPI_COOLDOWN_PACKET_T);  // Zmienione na 4
    MPI_Type_commit(&MPI_COOLDOWN_PACKET_T);
}

int assign_city() {
    // More sophisticated city assignment that considers cooldowns
    for (int i = 0; i < M; i++) {
        int city = (rank + i) % M;
        if (COOLDOWN_TIME[city] == 0) {
            return city;
        }
    }
    return rand() % M; // Fallback if all cities are cooling down
}

void send_to_all(packet_t *pkt, int tag) {
    for (int dest = 0; dest < N; dest++) {
        if (dest != rank) {
            MPI_Send(pkt, 1, MPI_PAKIET_T, dest, tag, MPI_COMM_WORLD);
        }
    }
}

void broadcast_cooldown_update(int city, int cooldown) {
    cooldown_packet_t pkt = {city, cooldown, lamport_clock, rank};  // Dodany owner
    for (int dest = 0; dest < N; dest++) {
        if (dest != rank) {
            MPI_Send(&pkt, 1, MPI_COOLDOWN_PACKET_T, dest, COOLDOWN_UPDATE, MPI_COMM_WORLD);
        }
    }
}

bool add_to_queue(int src, int lamport, int city) {
    if (queue_sizes[city] >= MAX_QUEUE_SIZE) {
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

void delete_from_queue(int src, int city) {
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

int compare_requests(request_t a, request_t b) {
    if (a.lamport != b.lamport) {
        return a.lamport - b.lamport;
    }

    return a.src - b.src;
}

void sort_queue(int city) {
    for (int i = 0; i < queue_sizes[city]-1; i++) {
        for (int j = 0; j < queue_sizes[city]-i-1; j++) {
            if (compare_requests(city_queues[city][j], city_queues[city][j+1]) > 0) {
                request_t temp = city_queues[city][j];
                city_queues[city][j] = city_queues[city][j+1];
                city_queues[city][j+1] = temp;
            }
        }
    }
}


void process_queue(int city) {
    sort_queue(city);
    
    for (int i = 0; i < queue_sizes[city]; i++) {
        request_t req = city_queues[city][i];
            packet_t ack_pkt = {rank, lamport_clock, city, ACK};
            MPI_Send(&ack_pkt, 1, MPI_PAKIET_T, req.src, ACK, MPI_COMM_WORLD);
            delete_from_queue(req.src, city);
            i--; // Ponieważ usunęliśmy element
    }
}

void update_cooldowns() {
    bool updated = FALSE;
    for (int city = 0; city < M; city++) {
        if (COOLDOWN_TIME[city] > 0 && COOLDOWN_OWNER[city] == rank) {
            updated = TRUE;
            COOLDOWN_TIME[city]--;
            
            if (COOLDOWN_TIME[city] == 0) {
                COOLDOWN_OWNER[city] = -1;
                broadcast_cooldown_update(city, 0);
                
            }
            else {
                broadcast_cooldown_update(city, COOLDOWN_TIME[city]);
            }
        }
    }
    if(updated) {
        lamport_clock++;
    }
}

void handle_cooldown_update(cooldown_packet_t *pkt) {
    lamport_clock = (pkt->lamport > lamport_clock) ? pkt->lamport : lamport_clock;
    lamport_clock++;
    
    COOLDOWN_TIME[pkt->city] = pkt->cooldown;
    COOLDOWN_OWNER[pkt->city] = pkt->owner;
    
    // if (pkt->cooldown == 0) {
    //     // Jeśli czekaliśmy na to miasto i mamy już wszystkie ACK
    //     if (state == WAITING && my_city == pkt->city && ack_count == N-1) {
    //         enter_city();
    //     }
    //     process_queue(pkt->city);
    // }
}

void enter_city() {
    int stay_time = rand() % (MAX_STAY - MIN_STAY + 1) + MIN_STAY;
    printf("\033[1;33m[%d] ENTERING city %d (clock: %d, stay: %d)\033[0m\n", 
           rank, my_city, lamport_clock, stay_time);
    
    state = IN_CITY;
    int start_clock = lamport_clock;
    
    while (lamport_clock - start_clock < stay_time) {
        update_cooldowns();
        sleep(1);
        lamport_clock++;
    }
    
    // Leave city
    int cooldown = rand() % (MAX_COOLDOWN - MIN_COOLDOWN + 1) + MIN_COOLDOWN;
    COOLDOWN_TIME[my_city] = cooldown;
    COOLDOWN_OWNER[my_city] = rank;  // Ustaw siebie jako właściciela cooldown
    broadcast_cooldown_update(my_city, cooldown);

    printf("\033[1;32m[%d] LEAVING city %d (clock: %d cooldown: %d)\033[0m\n", 
           rank, my_city, lamport_clock, cooldown);
    
   
    sleep(1);
    process_queue(my_city);
    
    state = IDLE;
    ack_count = 0;
    my_city = -1;
    my_request_lamport = -1;
}

void handle_ack(packet_t *pkt) {
    lamport_clock = (pkt->lamport > lamport_clock) ? pkt->lamport : lamport_clock;
    lamport_clock++;
    
    if (pkt->city != my_city || state != REQUESTING) {
        return; // ACK dla niewłaściwego miasta lub nie jesteśmy w stanie REQUESTING
    }
    
    ack_count++;

    if (ack_count == N-1) {
        if (COOLDOWN_TIME[my_city] == 0) {
            enter_city();
        } else {
            state = WAITING;
        }
    }
}

void handle_req(packet_t *pkt) {
    int req_lamport_clock = pkt->lamport;
    lamport_clock = (pkt->lamport > lamport_clock) ? pkt->lamport : lamport_clock;
    lamport_clock++;
    
    int city = pkt->city;
    
    // Always ACK if we're not interested in this city
    if (state == IDLE || 
    (state == REQUESTING && (city != my_city)) ||
    (state == IN_CITY && city != my_city) ||
    (state == REQUESTING && city == my_city &&
    (pkt->lamport < my_request_lamport ||
    (pkt->lamport == my_request_lamport && pkt->src < rank)))) {
        
        packet_t ack_pkt = {rank, lamport_clock, city, ACK};
        MPI_Send(&ack_pkt, 1, MPI_PAKIET_T, pkt->src, ACK, MPI_COMM_WORLD);
    } 
    // Otherwise queue the request
    else {
        add_to_queue(pkt->src, pkt->lamport, city);
    }
}

void receive_messages() {
    int flag;
    MPI_Status status;
    
    while (1) {
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
        if (!flag) break;

        if (status.MPI_TAG == REQ || status.MPI_TAG == ACK) {
            packet_t pkt;
            MPI_Recv(&pkt, 1, MPI_PAKIET_T, status.MPI_SOURCE, status.MPI_TAG, MPI_COMM_WORLD, &status);
            
            if (pkt.type == REQ) {
                handle_req(&pkt);
            } else {
                handle_ack(&pkt);
            }
        } 
        else if (status.MPI_TAG == COOLDOWN_UPDATE) {
            cooldown_packet_t pkt;
            MPI_Recv(&pkt, 1, MPI_COOLDOWN_PACKET_T, status.MPI_SOURCE, status.MPI_TAG, MPI_COMM_WORLD, &status);
            handle_cooldown_update(&pkt);
        }
    }
}

void make_request() {
    state = REQUESTING;
    my_city = assign_city();
    my_request_lamport = ++lamport_clock;
    ack_count = 0;
    
    packet_t req_pkt = {rank, my_request_lamport, my_city, REQ};
    send_to_all(&req_pkt, REQ);
    
    printf("\033[1;35m[%d] REQUESTING city %d (clock: %d)\033[0m\n", 
           rank, my_city, lamport_clock);
}

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &N);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    
    init_packet_type();
    init_cooldown_packet_type();
    srand(time(0) + rank);
    
    while (1) {
    update_cooldowns();
    
    if (state == IDLE && (rand() % 5) == 0) {
        make_request();
    }
    else if (state == WAITING) {
        // Sprawdź czy cooldown się zakończył (na wypadek gdybyśmy nie dostali update)
        if (COOLDOWN_TIME[my_city] == 0 && ack_count == N-1) {
            enter_city();
        }
    }
    
    receive_messages();
    sleep(1);
}
    
    MPI_Type_free(&MPI_PAKIET_T);
    MPI_Type_free(&MPI_COOLDOWN_PACKET_T);
    MPI_Finalize();
    return 0;
}