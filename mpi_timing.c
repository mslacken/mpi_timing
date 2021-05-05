/*
 * Copyright (C) 2021 SUSE
 *
 * This file is part of mpi_timing.
 *
 * Tlog is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpi_timing is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with tlog; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#define MAGIC_START 232323
#define MAGIC_END   424242
#define MAGIC_ID    123123

#define MAX_STR_SIZE 256;

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <mpi.h>

#include <gsl/gsl_statistics.h>
#include <gsl/gsl_sort.h>

#include "tlog/timespec.h"


static int world_rank = 0;
static int world_size = 0;

int int_pow(int base, int exp) {
    int result = 1;
    for (;;)
    {
        if (exp & 1)
            result *= base;
        exp >>= 1;
        if (!exp)
            break;
        base *= base;
    }

    return result;
}

enum run_mode {
  round_trip,
  round_trip_msg_size,
  round_trip_wait,
  round_trip_sync,
};

struct settings {
  unsigned int nr_runs;
  unsigned fill_random;
  enum run_mode mode;
};

void usage(struct settings mysettings) {
  printf("\tUsage: mpi_init [-rh] MODE\n");
  printf("\tperform small MPI timing test\n");
  printf("\t-h print this help\n");
  printf("\t-r initialize data with (pseudo) random values\n");
  printf("\t-s SEED set random seed\n");
  printf("\t-t TIMES how many times to run the test, default is %i\n",mysettings.nr_runs);
  printf("\t-w MSEC to wait after every round trip, default it %i\n",mysettings.wait);
  printf("\tMODE can be 'round_trip', 'round_trip_msg_size', 'round_trip_wait' and\n\t'round_trip_sync'\n");
  printf("\n");
  exit(EXIT_SUCCESS);
}

struct settings parse_cmdline(int argc,char** argv) {
  struct settings mysettings;
  mysettings.nr_runs = 1000;
  mysettings.fill_random = 0;
  mysettings.mode = round_trip;
  mysettings.wait = 20;
  int opt = 0;
  srand(42);
  while((opt = getopt(argc,argv,"rhs:t:w:")) != -1 ) {
    switch(opt) {
      case 'r':
        mysettings.fill_random = 1;
        break;
      case 'h':
        usage(mysettings);
        break;
      case 's':
        srand(atoi(optarg));
        break;
      case 't':
        mysettings.nr_runs = (atoi(optarg));
        break;
      case 'w':
        mysettings.wait = (atoi(optarg));
        break;
    }
  }
  for(; optind < argc; optind++){ //when some extra arguments are passed
    if (strcmp("round_trip",argv[optind]) == 0) 
      mysettings.mode = round_trip;
    if (strcmp("round_trip_msg_size",argv[optind]) == 0) 
      mysettings.mode = round_trip_msg_size;
    if (strcmp("round_trip_sync",argv[optind]) == 0) 
      mysettings.mode = round_trip_wait;
    if (strcmp("round_trip_wait",argv[optind]) == 0) 
      mysettings.mode = round_trip_sync;
  }
  return mysettings;
}

void round_trip_func(const unsigned int msg_size,struct timespec *snd_time, 
    struct timespec *rcv_time,int tag) {
  assert(msg_size >= 3);
  int * data = calloc(msg_size,sizeof(int));
  data[0] = MAGIC_START; data[msg_size-1] = MAGIC_END;
  data[1] = tag;
  int msg_id = MAGIC_ID;
  struct timespec time_start, time_end;
  if(world_rank != 0) {
    clock_gettime(CLOCK_MONOTONIC, &time_start);
    MPI_Recv(data,msg_size,MPI_INT,
        world_rank - 1,msg_id,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
    clock_gettime(CLOCK_MONOTONIC, &time_end);
    tlog_timespec_sub(&time_end,&time_start,rcv_time);
  } else {
    if(tag == -1) {
      for(unsigned int i = 2; i < msg_size-1; i++) {
        data[i] = rand();
      }
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &time_start);
  MPI_Send(data,msg_size,MPI_INT,
      (world_rank + 1) % world_size,msg_id,MPI_COMM_WORLD);
  clock_gettime(CLOCK_MONOTONIC, &time_end);
  tlog_timespec_sub(&time_end,&time_start,snd_time );
  if(world_rank == 0) {
    clock_gettime(CLOCK_MONOTONIC, &time_start);
    MPI_Recv(data,msg_size,MPI_INT,
        world_size-1,msg_id,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
    clock_gettime(CLOCK_MONOTONIC, &time_end);
    tlog_timespec_sub(&time_end,&time_start,rcv_time);
  }

  free(data);
}

void round_trip_sync_func(const unsigned int msg_size,struct timespec *snd_time, 
    struct timespec *rcv_time,int tag) {
  if(MPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    fprintf(stderr,"Barrier was not successfull on rank %i\n",world_rank);
    MPI_Abort(MPI_COMM_WORLD);
    exit(EXIT_FAILURE);
  }
  round_trip_func(msg_size,snd_time,rcv_time,tag);
}

void round_trip_wait_func(const unsigned int msg_size,struct timespec *snd_time, 
    struct timespec *rcv_time,int tag,unsigned int wait) {
  usleep(wait);
  round_trip_func(msg_size,snd_time,rcv_time,tag);
}

void round_trip_msg_size_func(const unsigned int msg_size,struct timespec *snd_time, 
    struct timespec *rcv_time,struct timespec* probe_time,int tag) {
  assert(msg_size >= 3);
  int * data = calloc(msg_size,sizeof(int));
  data[0] = MAGIC_START; data[msg_size-1] = MAGIC_END;
  data[1] = tag;
  int msg_id = MAGIC_ID, msg_size_status = 0;
  MPI_Status status;
  struct timespec time_start, time_end ;
  if(world_rank != 0) {
    clock_gettime(CLOCK_MONOTONIC, &time_start);
    MPI_Probe(world_rank-1,msg_id,MPI_COMM_WORLD,&status);
    clock_gettime(CLOCK_MONOTONIC, &time_end);
    tlog_timespec_sub(&time_end,&time_start,probe_time);

    MPI_Get_count(&status,MPI_INT,&msg_size_status);
    if(msg_size_status != (int) msg_size) {
      fprintf(stderr,"Messages sizes differs on rank %i: %i <-> %i\n",
          world_rank,msg_size_status,msg_size);
      exit(EXIT_FAILURE);
    }
    clock_gettime(CLOCK_MONOTONIC, &time_start);
    MPI_Recv(data,msg_size,MPI_INT,
        world_rank - 1,msg_id,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
    clock_gettime(CLOCK_MONOTONIC, &time_end);
    tlog_timespec_sub(&time_end,&time_start,rcv_time);
  } else {
    if(tag == -1) {
      for(unsigned int i = 2; i < msg_size-1; i++) {
        data[i] = rand();
      }
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &time_start);
  MPI_Send(data,msg_size,MPI_INT,
      (world_rank + 1) % world_size,msg_id,MPI_COMM_WORLD);
  clock_gettime(CLOCK_MONOTONIC, &time_end);
  tlog_timespec_sub(&time_end,&time_start,snd_time );
  if(world_rank == 0) {
    clock_gettime(CLOCK_MONOTONIC, &time_start);
    MPI_Probe(world_size-1,msg_id,MPI_COMM_WORLD,&status);
    clock_gettime(CLOCK_MONOTONIC, &time_end);
    tlog_timespec_sub(&time_end,&time_start,probe_time);

    MPI_Get_count(&status,MPI_INT,&msg_size_status);
    if(msg_size_status != (int) msg_size) {
      fprintf(stderr,"Messages sizes differs on rank %i: %i <-> %i\n",
          world_rank,msg_size_status,msg_size);
      exit(EXIT_FAILURE);
    }

    clock_gettime(CLOCK_MONOTONIC, &time_start);
    MPI_Recv(data,msg_size,MPI_INT,
        world_size-1,msg_id,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
    clock_gettime(CLOCK_MONOTONIC, &time_end);
    tlog_timespec_sub(&time_end,&time_start,rcv_time);
  }

  free(data);
}

int main(int argc, char** argv) {
  struct timespec time_start, time_end, time_diff, time_gl_start, time_gl_end,time_gl_diff; 
  struct settings mysettings = parse_cmdline(argc,argv);

  clock_gettime(CLOCK_MONOTONIC, &time_gl_start);
  clock_gettime(CLOCK_MONOTONIC, &time_start);
  MPI_Init(&argc,&argv);
  clock_gettime(CLOCK_MONOTONIC, &time_end);
  // Get the number of processes
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);

  MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
  char processor_name[MPI_MAX_PROCESSOR_NAME];
  int name_len;
  MPI_Get_processor_name(processor_name, &name_len);

  /* start with time which was needed for init and some more general information*/
  long* send_bf_init = malloc(2*sizeof(long));
  tlog_timespec_sub(&time_end,&time_start,&time_diff);
  send_bf_init[0] = time_diff.tv_sec; 
  send_bf_init[1] = time_diff.tv_nsec;
  if (world_rank == 0 ) {
    int mpi_version_len = 0;
    char mpi_version[MPI_MAX_LIBRARY_VERSION_STRING];
    MPI_Get_library_version(mpi_version,&mpi_version_len);
    printf("# MPI version: %s\n",mpi_version);
    printf("# Nr of processors are: %i\n",world_size);
    long *recv_bf_init = malloc(2*world_size*sizeof(long));
    char *recv_bf_proc = malloc(world_size*sizeof(char)*MPI_MAX_PROCESSOR_NAME);
    MPI_Gather(send_bf_init,2,MPI_LONG,
        recv_bf_init,2,MPI_LONG,0,MPI_COMM_WORLD);
    /* get the processor (node) names and print them out */
    MPI_Gather(processor_name,MPI_MAX_PROCESSOR_NAME,MPI_CHAR,
        recv_bf_proc,MPI_MAX_PROCESSOR_NAME,MPI_CHAR,0,MPI_COMM_WORLD);
    for(unsigned int i = 0; i < (unsigned int) world_size; i++) {
      char temp_str[MPI_MAX_PROCESSOR_NAME];
      strncpy(temp_str,&recv_bf_proc[MPI_MAX_PROCESSOR_NAME*i],MPI_MAX_PROCESSOR_NAME);
      if(strlen(temp_str) > 0) {
        printf("# %s:",temp_str);
        for(unsigned int j = i; j < (unsigned int) world_size; j++) {
          if(strcmp(temp_str,&recv_bf_proc[MPI_MAX_PROCESSOR_NAME*j])== 0) {
            printf(" %i",j);
            memset(&recv_bf_proc[MPI_MAX_PROCESSOR_NAME*j],'\0',MPI_MAX_PROCESSOR_NAME);
          }
        }
        printf("\n");
      }
    }
    printf("# MPI_Init times for ranks\n");
    for(unsigned int i = 0; i < (unsigned int) world_size; i++) {
      printf("# %lu.%lu\n",recv_bf_init[2*i],recv_bf_init[2*i+1]);
    }

    free(recv_bf_init);
    free(recv_bf_proc);
  } else {
    MPI_Gather(send_bf_init,2,MPI_LONG,NULL,2,MPI_LONG,0,MPI_COMM_WORLD);
    MPI_Gather(processor_name,MPI_MAX_PROCESSOR_NAME,MPI_CHAR,NULL,MPI_MAX_PROCESSOR_NAME,MPI_CHAR,0,MPI_COMM_WORLD);
  }
  free(send_bf_init);

  unsigned int pkg_size = 2, msg_count = 0;
  for(unsigned int i = 4; i <= 14;) {
    /* not only package size of 2 4 8, but 2 3 4 6 8 ... */
      if(pkg_size <(unsigned int) int_pow(2,i) ) {
        pkg_size = int_pow(2,i);
      } else {
        pkg_size += int_pow(2,i+1); 
        pkg_size /= 2;
        i++;
    }
    double *times_snd = calloc(mysettings.nr_runs,sizeof(double));
    double *times_rcv = calloc(mysettings.nr_runs,sizeof(double));
    double *times_prb = calloc(mysettings.nr_runs,sizeof(double));
    for(unsigned int j = 0; j < mysettings.nr_runs; j++) {
      /* now start with the ring test */
      struct timespec time_rcv, time_snd, time_probe; 
      time_probe.tv_sec = 0; time_probe.tv_nsec = 0;
      switch(mysettings.mode) {
        case round_trip:
          round_trip_func(pkg_size,&time_snd,&time_rcv,msg_count);
          msg_count++;
          break;
        case round_trip_msg_size:
          round_trip_msg_size_func(pkg_size,&time_snd,&time_rcv,&time_probe,msg_count);
          msg_count++;
          break;
        case round_trip_sync:
          round_trip_msg_size_sync_func(pkg_size,&time_snd,&time_rcv,&time_probe,msg_count);
          msg_count++;
          break;
        case round_trip_wait:
          round_trip_msg_size_sync_func(pkg_size,&time_snd,&time_rcv,&time_probe,msg_count
              ,mysettings.wait_time);
          msg_count++;
          break;
        default:
          fprintf(stderr,"Invalid mode selected\n");
          exit(EXIT_FAILURE);

      }
      times_snd[j] = tlog_timespec_to_fp(&time_snd);
      times_rcv[j] = tlog_timespec_to_fp(&time_rcv);
      times_prb[j] = tlog_timespec_to_fp(&time_probe);
    }
    gsl_sort(times_snd,1,mysettings.nr_runs); gsl_sort(times_rcv,1,mysettings.nr_runs);
    gsl_sort(times_prb,1,mysettings.nr_runs);
    double send_bf[15] = {
        gsl_stats_max(times_snd,1,mysettings.nr_runs),gsl_stats_min(times_snd,1,mysettings.nr_runs),
        gsl_stats_mean(times_snd,1,mysettings.nr_runs),gsl_stats_median_from_sorted_data(times_snd,1,mysettings.nr_runs),
        gsl_stats_variance(times_snd,1,mysettings.nr_runs),
        gsl_stats_max(times_rcv,1,mysettings.nr_runs),gsl_stats_min(times_rcv,1,mysettings.nr_runs),
        gsl_stats_mean(times_rcv,1,mysettings.nr_runs),gsl_stats_median_from_sorted_data(times_rcv,1,mysettings.nr_runs),
        gsl_stats_variance(times_rcv,1,mysettings.nr_runs),
        gsl_stats_max(times_prb,1,mysettings.nr_runs),gsl_stats_min(times_prb,1,mysettings.nr_runs),
        gsl_stats_mean(times_prb,1,mysettings.nr_runs),gsl_stats_median_from_sorted_data(times_prb,1,mysettings.nr_runs),
        gsl_stats_variance(times_prb,1,mysettings.nr_runs) };
    if (world_rank == 0 ) {
      double *recv_bf = calloc(world_size * 15,sizeof(double));
      clock_gettime(CLOCK_MONOTONIC, &time_start);
      MPI_Gather(send_bf,15,MPI_DOUBLE,recv_bf,15,MPI_DOUBLE,0,MPI_COMM_WORLD);
      clock_gettime(CLOCK_MONOTONIC, &time_end);
      tlog_timespec_sub(&time_end,&time_start,&time_diff);
      printf("%i %g\n",world_rank,send_bf[12]);
      printf("# Time for gather %lu.%lu\n",time_diff.tv_sec,time_diff.tv_nsec);
      printf("%i",pkg_size);
      printf(" %g %g %g %g %g",
          gsl_stats_max(&recv_bf[0],15,world_size),
          gsl_stats_min(&recv_bf[1],15,world_size),
          gsl_stats_mean(&recv_bf[2],15,world_size),
          gsl_stats_mean(&recv_bf[3],15,world_size),
          gsl_stats_mean(&recv_bf[4],15,world_size));
      printf(" %g %g %g %g %g",
          gsl_stats_max(&recv_bf[5],15,world_size),
          gsl_stats_min(&recv_bf[6],15,world_size),
          gsl_stats_mean(&recv_bf[7],15,world_size),
          gsl_stats_mean(&recv_bf[8],15,world_size),
          gsl_stats_mean(&recv_bf[9],15,world_size));
      printf(" %g %g %g %g %g",
          gsl_stats_max(&recv_bf[10],15,world_size),
          gsl_stats_min(&recv_bf[11],15,world_size),
          gsl_stats_mean(&recv_bf[12],15,world_size),
          gsl_stats_mean(&recv_bf[13],15,world_size),
          gsl_stats_mean(&recv_bf[14],15,world_size));
      printf(" %lu %lu %lu",
          (gsl_stats_max_index(&recv_bf[2],15,world_size)),
          (gsl_stats_max_index(&recv_bf[7],15,world_size)),
          (gsl_stats_max_index(&recv_bf[11],15,world_size)));
      printf("\n");
      free(recv_bf);
    } else {             
      MPI_Gather(send_bf,15,MPI_DOUBLE,NULL,15,MPI_DOUBLE,0,MPI_COMM_WORLD);
    }
  }



  clock_gettime(CLOCK_MONOTONIC, &time_start);
  MPI_Finalize();
  clock_gettime(CLOCK_MONOTONIC, &time_end);
  tlog_timespec_sub(&time_end,&time_start,&time_diff);
  printf("# MPI_Finalize[%i]: %li.%li\n",world_rank,time_diff.tv_sec,time_diff.tv_nsec);
  clock_gettime(CLOCK_MONOTONIC, &time_gl_end);
  tlog_timespec_sub(&time_gl_end,&time_gl_start,&time_gl_diff);
  printf("# Total run time [%i]: %li.%li\n",world_rank,time_gl_diff.tv_sec,time_gl_diff.tv_nsec);
  exit(EXIT_SUCCESS);
}
