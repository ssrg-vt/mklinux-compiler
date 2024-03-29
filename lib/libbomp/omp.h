/*
 * Copyright (c) 2016 Virginia Tech,
 * Author: bielsk1@vt.edu
 * All rights reserved.
 */

#ifndef OMP_H
#define OMP_H

void bomp_env_init(void);
void backend_span_domain(int nos_threads, size_t stack_size);
void backend_span_domain_default(int nos_threads);
void backend_create_time(int cores);

void bomp_custom_init(void);
void bomp_custom_exit(void);
void omp_set_num_threads (int);
int omp_get_num_threads (void);
int omp_get_max_threads (void);
int omp_get_thread_num (void);
int omp_get_num_procs (void);
int omp_in_parallel (void);

void omp_set_dynamic (int);
int omp_get_dynamic (void);

void omp_set_nested (int);
int omp_get_nested (void);

#if 0
void omp_init_lock (omp_lock_t *);
void omp_destroy_lock (omp_lock_t *);
void omp_set_lock (omp_lock_t *);
void omp_unset_lock (omp_lock_t *);
int omp_test_lock (omp_lock_t *);

void omp_init_nest_lock (omp_nest_lock_t *);
void omp_destroy_nest_lock (omp_nest_lock_t *);
void omp_set_nest_lock (omp_nest_lock_t *);
void omp_unset_nest_lock (omp_nest_lock_t *);
int omp_test_nest_lock (omp_nest_lock_t *);
#endif

double omp_get_wtime (void);
double omp_get_wtick (void);

void bomp_synchronize(void);
#endif /* OMP_H */
