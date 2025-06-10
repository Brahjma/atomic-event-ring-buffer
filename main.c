#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

// Incluir tu Ring Buffer
#include "atomic_event_ring_buffer.c"

#define NUM_PRODUCERS 8       // Más productores para saturar
#define NUM_CONSUMERS 2       // Menos consumidores para desbalance
#define EVENTS_PER_PRODUCER 500000 // Medio millón por productor
#define CONSUMER_DELAY_US 10  // Retraso para simular VMM lento

// Contadores globales para verificación
atomic_int total_produced = 0;
atomic_int total_consumed = 0;

// --- PRODUCTOR ---
void* producer_thread(void* arg) {
    long thread_id = (long)arg;
    int success_count = 0;
    syslog(LOG_INFO, "Producer %ld started", thread_id);

    for (long i = 0; i < EVENTS_PER_PRODUCER; i++) {
        Event event = {
            .event_id = ((uint64_t)thread_id << 32) | i,
            .pid = (uint32_t)(thread_id + 1000),
            .vpn = (uint32_t)(i % 1024)
        };
        if (enqueue_event(&global_ring_buffer, &event) == 0) {
            success_count++;
            atomic_fetch_add(&total_produced, 1);
        } else {
            usleep(1); // Backpressure
        }
    }

    syslog(LOG_INFO, "Producer %ld finished: %d events", thread_id, success_count);
    return (void*)(long)success_count;
}

// --- CONSUMIDOR ---
void* consumer_thread(void* arg) {
    long thread_id = (long)arg;
    int success_count = 0;
    Event event;
    syslog(LOG_INFO, "Consumer %ld started", thread_id);

    while (success_count < (EVENTS_PER_PRODUCER * NUM_PRODUCERS) / NUM_CONSUMERS) {
        if (dequeue_event(&global_ring_buffer, &event) == 0) {
            // Verificar integridad
            uint32_t expected_pid = (event.event_id >> 32) + 1000;
            if (event.pid != expected_pid) {
                syslog(LOG_ERR, "Consumer %ld: Corrupted event %lu (PID %u, expected %u)",
                       thread_id, event.event_id, event.pid, expected_pid);
            }
            success_count++;
            atomic_fetch_add(&total_consumed, 1);
            usleep(CONSUMER_DELAY_US); // Simular VMM lento
        } else {
            usleep(1);
        }
    }

    syslog(LOG_INFO, "Consumer %ld finished: %d events", thread_id, success_count);
    return (void*)(long)success_count;
}

// --- MAIN ---
int main(void) {
    openlog("RingBufferStress", LOG_PID|LOG_CONS, LOG_USER);
    printf("--- Stress Testing Ring Buffer ---\n");

    ring_buffer_init(&global_ring_buffer);

    pthread_t producers[NUM_PRODUCERS], consumers[NUM_CONSUMERS];
    long total_success_produced = 0, total_success_consumed = 0;

    // Crear productores
    for (long i = 0; i < NUM_PRODUCERS; i++) {
        pthread_create(&producers[i], NULL, producer_thread, (void*)i);
    }

    // Crear consumidores
    for (long i = 0; i < NUM_CONSUMERS; i++) {
        pthread_create(&consumers[i], NULL, consumer_thread, (void*)i);
    }

    // Esperar productores
    for (long i = 0; i < NUM_PRODUCERS; i++) {
        long count;
        pthread_join(producers[i], (void**)&count);
        total_success_produced += count;
    }

    // Esperar consumidores
    for (long i = 0; i < NUM_CONSUMERS; i++) {
        long count;
        pthread_join(consumers[i], (void**)&count);
        total_success_consumed += count;
    }

    // Verificar estado final
    uint64_t head = atomic_load_explicit(&global_ring_buffer.head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&global_ring_buffer.tail, memory_order_relaxed);

    printf("\n--- Test Summary ---\n");
    printf("Produced: %d, Consumed: %d\n", total_success_produced, total_success_consumed);
    printf("Final state: Head=%lu, Tail=%lu\n", head, tail);

    if (total_success_produced == total_success_consumed && head == tail) {
        printf("SUCCESS: All events processed correctly\n");
    } else {
        printf("FAILURE: Inconsistent state\n");
    }

    closelog();
    return 0;
}
