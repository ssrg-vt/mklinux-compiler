#include "pthread_impl.h"
#include <threads.h>
#include <pthread.h>
#include "libc.h"

/* Popcorn migration data access */
void** pthread_migrate_args()
{
	//pthread_t self = __pthread_self();
	struct pthread *self = __pthread_self();
        return &(self->__args);
}

