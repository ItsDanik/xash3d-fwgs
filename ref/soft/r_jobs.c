/*
r_jobs.c - a second thread for the parts of a frame that can be shared
Copyright (C) 2026 Danik

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "r_local.h"

#ifdef MISTER_HYBRID
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <unistd.h>

/*
The MiSTer has two ARM cores. The renderer hands out work that touches pixels
of its own and nothing else (the spans of one surface, a part of the rows of
the frame for the window) as jobs: the helper thread on the second core and
the renderer's own thread both take them from the list until it is empty.

The helper only runs between R_JobsBegin() and R_JobsFinish(), and sleeps
otherwise: Main_MiSTer lives on that core too.
*/

#define MAX_JOBS 1024

typedef struct
{
	rjob_t fn;
	byte   arg[RJOB_ARG_SIZE];
} job_t;

static job_t     jobs[MAX_JOBS];
static int       job_head;  // next one to take
static int       job_tail;  // one past the last that is there
static int       job_done;  // how many are finished
static int       job_active;
static qboolean  job_thread; // there is a helper
static qboolean  job_tried;
static sem_t     job_wake;
static pthread_t job_pthread;

#define LOAD( x )     __atomic_load_n( &( x ), __ATOMIC_SEQ_CST )
#define STORE( x, v ) __atomic_store_n( &( x ), ( v ), __ATOMIC_SEQ_CST )

// false: none left
static qboolean R_JobTake( void )
{
	int i = LOAD( job_head );

	if( i >= LOAD( job_tail ))
		return false;

	if( __atomic_compare_exchange_n( &job_head, &i, i + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
	{
		jobs[i].fn( jobs[i].arg );
		__atomic_add_fetch( &job_done, 1, __ATOMIC_SEQ_CST );
	}
	return true;
}

static void *R_JobThread( void *unused )
{
	for( ;; )
	{
		sem_wait( &job_wake );

		while( LOAD( job_active ))
		{
			if( !R_JobTake( ))
				sched_yield(); // Main_MiSTer gets a turn while the renderer makes the next job
		}
	}
	return NULL;
}

static void R_JobsInit( void )
{
	cpu_set_t      cpus;
	pthread_attr_t attr;

	job_tried = true;

	// the renderer's thread has the first core, the helper gets the other one
	if( sysconf( _SC_NPROCESSORS_ONLN ) < 2 || getenv( "MISTER_HYBRID_NOJOBS" ))
		return;
	sem_init( &job_wake, 0, 0 );
	pthread_attr_init( &attr );
	CPU_ZERO( &cpus );
	CPU_SET( 1, &cpus );
	pthread_attr_setaffinity_np( &attr, sizeof( cpus ), &cpus );
	job_thread = pthread_create( &job_pthread, &attr, R_JobThread, NULL ) == 0;
	pthread_attr_destroy( &attr );
	if( !job_thread ) // not allowed on that core: wherever there is room, then
		job_thread = pthread_create( &job_pthread, NULL, R_JobThread, NULL ) == 0;

	if( job_thread )
		gEngfuncs.Con_Printf( "ref_soft: helper thread on the second core\n" );
}

/*
================
R_JobsBegin

Jobs follow. Nothing they read may change before R_JobsFinish() or
R_JobsDrain() has returned.
================
*/
void R_JobsBegin( void )
{
	if( !job_tried )
		R_JobsInit();

	// in this order: the helper may still be looking at the list
	STORE( job_tail, 0 );
	STORE( job_head, 0 );
	STORE( job_done, 0 );

	if( job_thread )
	{
		STORE( job_active, 1 );
		sem_post( &job_wake );
	}
}

/*
================
R_JobAdd

fn( arg ) is called once, on either thread, with a copy of arg
================
*/
void R_JobAdd( rjob_t fn, const void *arg, size_t size )
{
	if( !LOAD( job_active ))
	{
		// no helper, or not between Begin and Finish
		fn( arg );
		return;
	}

	if( job_tail == MAX_JOBS )
	{
		R_JobsDrain();
		STORE( job_tail, 0 );
		STORE( job_head, 0 );
		STORE( job_done, 0 );
	}

	jobs[job_tail].fn = fn;
	memcpy( jobs[job_tail].arg, arg, size );
	STORE( job_tail, job_tail + 1 );
}

/*
================
R_JobsDrain

Returns when every job that was added is finished; this thread works too
================
*/
void R_JobsDrain( void )
{
	if( !LOAD( job_active ))
		return;

	while( R_JobTake( ))
		;

	// the helper may be in its last one
	while( LOAD( job_done ) < job_tail )
		;
}

void R_JobsFinish( void )
{
	R_JobsDrain();
	STORE( job_active, 0 );
}

#else // !MISTER_HYBRID

void R_JobsBegin( void )
{
}

void R_JobAdd( rjob_t fn, const void *arg, size_t size )
{
	fn( arg );
}

void R_JobsDrain( void )
{
}

void R_JobsFinish( void )
{
}

#endif // !MISTER_HYBRID
