#include <linux/can.h>
#include <linux/can/raw.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "can_manager.h"

#define MAX_RX_QUEUES 128

typedef struct can_rx_queue_t {
  struct canfd_frame frames[MAX_RX_QUEUES];
  int head;
  int tail;
  int count;

  pthread_mutex_t mutex;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
} can_rx_queue_t;


can_rx_queue_t *can_rx_queue_init(void)
{
  can_rx_queue_t *result = (can_rx_queue_t *)malloc(1 * sizeof(can_rx_queue_t));
  if (result == NULL) {
    return NULL;
  }

  result->head  = 0;
  result->tail  = 0;
  result->count = 0;

  pthread_mutex_init(&result->mutex, NULL);
  pthread_cond_init(&result->not_empty, NULL);
  pthread_cond_init(&result->not_full, NULL);

  return result;
}


void can_rx_queue_destroy(can_rx_queue_t *obj)
{
  if (obj == NULL) return;
  pthread_mutex_destroy(&obj->mutex);
  pthread_cond_destroy(&obj->not_empty);
  pthread_cond_destroy(&obj->not_full);
  free(obj);
}


int can_rx_queue_push(can_rx_queue_t *q, const struct canfd_frame *frame)
{
  pthread_mutex_lock(&q->mutex);
  while (q->count == MAX_RX_QUEUES) {
    pthread_cond_wait(&q->not_full, &q->mutex);
  }

  q->frames[q->tail] = *frame;
  q->tail            = (q->tail + 1) % MAX_RX_QUEUES;
  q->count++;

  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->mutex);

  return 0;
}

int can_rx_queue_pop(can_rx_queue_t *q, struct canfd_frame *frame)
{
  pthread_mutex_lock(&q->mutex);

  while (q->count == 0) {
    pthread_cond_wait(&q->not_empty, &q->mutex);
  }

  *frame  = q->frames[q->head];
  q->head = (q->head + 1) % MAX_RX_QUEUES;
  q->count--;

  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->mutex);
  return 0;
}


int can_rx_queue_try_push(can_rx_queue_t *q, const struct canfd_frame *frame)
{
  pthread_mutex_lock(&q->mutex);

  if (q->count == MAX_RX_QUEUES) {
    pthread_mutex_unlock(&q->mutex);
    return 1;
  }

  pthread_mutex_unlock(&q->mutex);

  struct canfd_frame copy = *frame;

  pthread_mutex_lock(&q->mutex);
  if (q->count == MAX_RX_QUEUES) {
    pthread_mutex_unlock(&q->mutex);
    return 1;
  }

  q->frames[q->tail] = copy;
  q->tail            = (q->tail + 1) % MAX_RX_QUEUES;
  q->count++;

  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->mutex);
  return 0;
}


int can_rx_queue_try_pop(can_rx_queue_t *q, struct canfd_frame *frame)
{
  pthread_mutex_lock(&q->mutex);

  if (q->count == 0) {
    pthread_mutex_unlock(&q->mutex);
    return 1;
  }

  *frame  = q->frames[q->head];
  q->head = (q->head + 1) % MAX_RX_QUEUES;
  q->count--;

  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->mutex);
  return 0;
}
