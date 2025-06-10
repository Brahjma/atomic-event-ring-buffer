#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h> // Para usleep
#include <time.h>   // Para srand y rand
#include <errno.h>  // Para manejar errores

// Incluir la implementación de nuestro ring buffer
#include "atomic_event_ring_buffer.c" // Esto es solo para simplificar en un solo archivo.
                                     // Normalmente, se incluiría el .h y se compilaría el .c por separado.

#define NUM_PRODUCERS 2
#define NUM_CONSUMERS 2
#define EVENTS_PER_THREAD 1000000 // Millones de eventos por hilo

// Instancia global del ring buffer
AtomicEventRingBuffer global_ring_buffer;

// --- Hilo Productor ---
void* producer_thread(void* arg) {
    long thread_id = (long)arg;
    int success_count = 0;
    int fail_count = 0;

    printf("Productor %ld: Iniciado.\n", thread_id);

    for (long i = 0; i < EVENTS_PER_THREAD; ++i) {
        Event event_to_enqueue = {
            .pid = (uint32_t)(thread_id * 1000 + (i % 1000)), // PID único por hilo y evento
            .vpn = (uint32_t)i                              // VPN simple
        };

        if (enqueue_event(&global_ring_buffer, &event_to_enqueue) == 0) {
            success_count++;
        } else {
            fail_count++;
            // En un escenario real de kernel, un fallo aquí podría significar
            // un reintento, una caída, o un manejo de backpressure más sofisticado.
            // Para la prueba, solo contamos.
            // usleep(1); // Pequeña pausa si está lleno para no saturar
        }
    }

    printf("Productor %ld: Finalizado. Añadidos: %d, Fallidos (Buffer Lleno): %d\n", thread_id, success_count, fail_count);
    return (void*)(long)success_count; // Retorna el conteo de éxitos
}

// --- Hilo Consumidor ---
void* consumer_thread(void* arg) {
    long thread_id = (long)arg;
    int success_count = 0;
    int fail_count = 0;
    Event received_event;

    printf("Consumidor %ld: Iniciado.\n", thread_id);

    // Los consumidores intentarán consumir hasta que los productores terminen
    // y el buffer esté completamente vacío. En un sistema real,
    // el consumidor podría esperar señales o tener un bucle infinito.
    // Aquí hacemos un número fijo de intentos si el buffer está vacío.
    long attempts_remaining = EVENTS_PER_THREAD * NUM_PRODUCERS * 2; // Intentos extra para vaciar el buffer

    while (attempts_remaining > 0 || success_count < (EVENTS_PER_THREAD * NUM_PRODUCERS) / NUM_CONSUMERS) {
        if (dequeue_event(&global_ring_buffer, &received_event) == 0) {
            success_count++;
            // printf("Consumidor %ld: Recibido PID=%u, VPN=%u\n", thread_id, received_event.pid, received_event.vpn);
        } else {
            fail_count++;
            // usleep(1); // Pequeña pausa si está vacío
        }
        attempts_remaining--;
        if (success_count >= (EVENTS_PER_THREAD * NUM_PRODUCERS) / NUM_CONSUMERS) {
            break; // Si ya consumimos nuestra parte proporcional, salimos.
        }
    }

    printf("Consumidor %ld: Finalizado. Consumidos: %d, Fallidos (Buffer Vacío): %d\n", thread_id, success_count, fail_count);
    return (void*)(long)success_count; // Retorna el conteo de éxitos
}

// --- Función Principal ---
int main() {
    srand(time(NULL)); // Inicializar generador de números aleatorios para los PID/VPNs de prueba

    printf("--- Iniciando prueba de Ring Buffer Lock-Free ---\n");

    // Inicializar el ring buffer
    ring_buffer_init(&global_ring_buffer);

    pthread_t producers[NUM_PRODUCERS];
    pthread_t consumers[NUM_CONSUMERS];
    long total_produced = 0;
    long total_consumed = 0;
    long i;

    // Crear hilos productores
    for (i = 0; i < NUM_PRODUCERS; ++i) {
        if (pthread_create(&producers[i], NULL, producer_thread, (void*)i) != 0) {
            perror("Error al crear hilo productor");
            return 1;
        }
    }

    // Crear hilos consumidores
    for (i = 0; i < NUM_CONSUMERS; ++i) {
        if (pthread_create(&consumers[i], NULL, consumer_thread, (void*)i) != 0) {
            perror("Error al crear hilo consumidor");
            return 1;
        }
    }

    // Esperar a que los hilos productores terminen
    for (i = 0; i < NUM_PRODUCERS; ++i) {
        long produced_by_thread;
        pthread_join(producers[i], (void**)&produced_by_thread);
        total_produced += produced_by_thread;
    }

    // Esperar a que los hilos consumidores terminen
    for (i = 0; i < NUM_CONSUMERS; ++i) {
        long consumed_by_thread;
        pthread_join(consumers[i], (void**)&consumed_by_thread);
        total_consumed += consumed_by_thread;
    }

    printf("\n--- Resumen de la Prueba ---\n");
    printf("Total de eventos intentados por productores: %ld\n", EVENTS_PER_THREAD * NUM_PRODUCERS);
    printf("Total de eventos añadidos exitosamente: %ld\n", total_produced);
    printf("Total de eventos consumidos exitosamente: %ld\n", total_consumed);

    // Un pequeño delay para asegurar que el buffer esté completamente procesado
    // antes de una posible verificación de head/tail final.
    usleep(100000); // 100 ms

    uint64_t final_head = atomic_load_explicit(&global_ring_buffer.head, memory_order_relaxed);
    uint64_t final_tail = atomic_load_explicit(&global_ring_buffer.tail, memory_order_relaxed);

    printf("Estado final del buffer: Head=%lu, Tail=%lu\n", final_head, final_tail);

    if (total_produced == total_consumed && final_head == final_tail) {
        printf("¡Prueba de Ring Buffer Lock-Free: ÉXITO! Todos los eventos procesados consistentemente.\n");
    } else {
        printf("¡Prueba de Ring Buffer Lock-Free: FALLO! Discrepancia en conteo o estado final.\n");
    }

    printf("--- Fin de la Demostración ---\n");

    return 0;
}
