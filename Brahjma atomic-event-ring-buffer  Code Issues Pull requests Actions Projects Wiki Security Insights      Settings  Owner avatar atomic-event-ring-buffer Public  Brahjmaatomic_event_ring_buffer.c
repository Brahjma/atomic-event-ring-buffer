#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> // Para usleep
#include <pthread.h> // Para hilos

// --- CONFIGURACIÓN DEL RING BUFFER ---
#define RING_SIZE 1024 // Tamaño del buffer
// Para evitar false sharing, alinear los punteros head y tail, y el buffer mismo.
// Un tamaño típico de línea de caché es 64 bytes.
#define CACHE_LINE_SIZE 64

// --- PRAGMAS PARA ALINEACIÓN (GCC/Clang) ---
#if defined(__GNUC__) || defined(__clang__)
#define ALIGNED(x) __attribute__ ((aligned(x)))
#else
#define ALIGNED(x)
#endif

// --- ESTRUCTURA DEL EVENTO (PARA VMM) ---
// Representa un evento de alto rendimiento, como una falla de página en un VMM.
typedef struct {
    uint32_t pid; // Process ID del guest que generó el evento
    uint32_t vpn; // Virtual Page Number asociado al evento
    // Un event_id podría ser (PID << 32) | index para unicidad y trazabilidad.
} Event;

// --- ESTRUCTURA DEL RING BUFFER MPMC (Lock-Free) ---
typedef struct {
    // Punteros atómicos para la cabeza y la cola.
    // Usamos _Atomic y memoria_order_ para garantizar concurrencia lock-free.
    // Alineados para prevenir false sharing.
    ALIGNED(CACHE_LINE_SIZE) atomic_uint_least64_t head; // Próxima posición para DEQUEUE
    ALIGNED(CACHE_LINE_SIZE) atomic_uint_least64_t tail; // Próxima posición para ENQUEUE

    // El buffer de eventos. También alineado.
    ALIGNED(CACHE_LINE_SIZE) Event buffer[RING_SIZE];
} AtomicEventRingBuffer;

// --- INICIALIZACIÓN ---
void ring_buffer_init(AtomicEventRingBuffer *rb) {
    atomic_store_explicit(&rb->head, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, 0, memory_order_relaxed);
    // No es necesario inicializar el contenido del buffer para lock-free.
    printf("Ring Buffer: Inicializado.\n");
}

// --- ENQUEUE (Productor) ---
// Añade un evento al buffer.
// MPMC: Múltiples productores pueden llamar a esta función simultáneamente.
// Retorna 0 en éxito, -1 si el buffer está lleno.
int enqueue_event(AtomicEventRingBuffer *rb, const Event *event) {
    uint64_t current_tail;
    uint64_t current_head;

    do {
        current_tail = atomic_load_explicit(&rb->tail, memory_order_relaxed); // Lee la cola (relajado)
        current_head = atomic_load_explicit(&rb->head, memory_order_acquire); // Lee la cabeza (acquire para visibilidad de escrituras de dequeue)

        // Calcula la siguiente posición para el tail.
        uint64_t next_tail = (current_tail + 1) % RING_SIZE;

        // Comprueba si el buffer está lleno.
        // Si el próximo tail es igual al head, el buffer está lleno.
        if (next_tail == current_head) {
            // El buffer está lleno. Opciones: spin-wait, yield, o retornar error.
            // Para ultra-baja latencia en kernel, spin-wait corto o yield es común.
            // __builtin_cpu_pause() para CPUs Intel/AMD reduce el consumo de energía en spin-waits.
            #ifdef __x86_64__
            __builtin_cpu_pause();
            #endif
            // printf("Ring Buffer: ENQUEUE - Buffer Lleno. current_tail=%lu, current_head=%lu\n", current_tail, current_head);
            return -1; // Retorna error si está lleno
        }

        // Intenta avanzar el tail atómicamente.
        // memory_order_release: Asegura que el evento escrito sea visible antes de que el tail avance.
    } while (!atomic_compare_exchange_weak_explicit(&rb->tail, &current_tail, next_tail, memory_order_release, memory_order_relaxed));

    // Si el CAS tuvo éxito, tenemos la posición de escritura.
    // Escribe el evento en la posición calculada.
    rb->buffer[current_tail % RING_SIZE] = *event;

    // printf("Ring Buffer: ENQUEUE - Evento añadido en %lu. PID=%u, VPN=%u\n", current_tail, event->pid, event->vpn);
    return 0; // Éxito
}

// --- DEQUEUE (Consumidor) ---
// Extrae un evento del buffer.
// MPMC: Múltiples consumidores pueden llamar a esta función simultáneamente.
// Retorna 0 en éxito, -1 si el buffer está vacío.
int dequeue_event(AtomicEventRingBuffer *rb, Event *event) {
    uint64_t current_head;
    uint64_t current_tail;

    do {
        current_head = atomic_load_explicit(&rb->head, memory_order_relaxed); // Lee la cabeza (relajado)
        current_tail = atomic_load_explicit(&rb->tail, memory_order_acquire); // Lee la cola (acquire para visibilidad de escrituras de enqueue)

        // Comprueba si el buffer está vacío.
        // Si head es igual a tail, el buffer está vacío.
        if (current_head == current_tail) {
            // El buffer está vacío. Opciones: spin-wait, yield, o retornar error.
            #ifdef __x86_64__
            __builtin_cpu_pause();
            #endif
            // printf("Ring Buffer: DEQUEUE - Buffer Vacío. current_head=%lu, current_tail=%lu\n", current_head, current_tail);
            return -1; // Retorna error si está vacío
        }

        // Si el buffer no está vacío, podemos leer el evento.
        // memory_order_acquire: Asegura que el evento leído sea visible antes de que el head avance.
        *event = rb->buffer[current_head % RING_SIZE];

        // Intenta avanzar el head atómicamente.
        // memory_order_release: Libera la ranura una vez que el evento ha sido leído y el head ha avanzado.
    } while (!atomic_compare_exchange_weak_explicit(&rb->head, &current_head, (current_head + 1) % RING_SIZE, memory_order_release, memory_order_relaxed));

    // printf("Ring Buffer: DEQUEUE - Evento extraído de %lu. PID=%u, VPN=%u\n", current_head, event->pid, event->vpn);
    return 0; // Éxito
}
