/**
 * @file main.c
 * @brief This file contains the source code of the application to parallelise.
 * @details This application is a classic heat spread simulation.
 * @author Ludovic Capelli
 **/

#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>
#include <omp.h>
#include <inttypes.h>
#include <math.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>

#include "util.h"

/**
 * @argv[0] Name of the program
 * @argv[1] path to the dataset to load
 **/
int main(int argc, char* argv[])
{
	MPI_Init(NULL, NULL);

	/////////////////////////////////////////////////////
	// -- PREPARATION 1: COLLECT USEFUL INFORMATION -- //
	/////////////////////////////////////////////////////
	// Ranks for convenience so that we don't throw raw values all over the code
	const int MASTER_PROCESS_RANK = 0;

	// The rank of the MPI process in charge of this instance
	int my_rank;
	MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);

	// Number of MPI processes in total, commonly called "comm_size" for "communicator size".
	int comm_size;
	MPI_Comm_size(MPI_COMM_WORLD, &comm_size);

	/// Rank of the first MPI process
	const int FIRST_PROCESS_RANK = 0;
	/// Rank of the last MPI process
	const int LAST_PROCESS_RANK = comm_size - 1;

	// Rank of my up neighbour if any
	int up_neighbour_rank = (my_rank == FIRST_PROCESS_RANK) ? MPI_PROC_NULL : my_rank - 1;
	
	// Rank of my down neighbour if any
	int down_neighbour_rank = (my_rank == LAST_PROCESS_RANK) ? MPI_PROC_NULL : my_rank + 1;

	// report_placement();

	////////////////////////////////////////////////////////////////////
	// -- PREPARATION 2: INITIALISE TEMPERATURES ON MASTER PROCESS -- //
	////////////////////////////////////////////////////////////////////
	/// Array that will contain my part chunk. It will include the 2 ghost rows (1 up, 1 down)
	double temperatures[ROWS_PER_MPI_PROCESS+2][COLUMNS_PER_MPI_PROCESS];
	/// Temperatures from the previous iteration, same dimensions as the array above.
	double temperatures_last[ROWS_PER_MPI_PROCESS+2][COLUMNS_PER_MPI_PROCESS];
	/// On master process only: contains all temperatures read from input file.
	double all_temperatures[ROWS][COLUMNS];

	// The master MPI process will read a chunk from the file, send it to the corresponding MPI process and repeat until all chunks are read.
	if(my_rank == MASTER_PROCESS_RANK)
	{
		// EC: For SMALL -> sets every 500th column (including col 0) to MAX_TEMPERATURE = 50.0
		// 	   For BIG   -> sets a 'cross' centered in the middle row and col of the dataset to MAX_TEMPERATURE = 50.0
		// 	   				so that the corners are 0.0.
		initialise_temperatures(all_temperatures); 
	}

	// Syncs all processes
	MPI_Barrier(MPI_COMM_WORLD);

	///////////////////////////////////////////
	//     ^                                 //
	//    / \                                //
	//   / | \    CODE FROM HERE IS TIMED    //
	//  /  o  \                              //
	// /_______\                             //
	///////////////////////////////////////////
	
	////////////////////////////////////////////////////////
	// -- TASK 1: DISTRIBUTE DATA TO ALL MPI PROCESSES -- //
	////////////////////////////////////////////////////////
	double total_time_so_far = 0.0;
	double start_time = MPI_Wtime();

	if(my_rank == MASTER_PROCESS_RANK)
	{
		for(int i = 0; i < comm_size; i++)
		{
			// Is the i'th chunk meant for me, the master MPI process?
			if(i != my_rank)
			{
				// No, so send the corresponding chunk to that MPI process.
				MPI_Ssend(&all_temperatures[i * ROWS_PER_MPI_PROCESS][0], ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, i, 0, MPI_COMM_WORLD);
			}
			else
			{
				// Yes, let's copy it straight for the array in which we read the file into.
				for(int j = 1; j <= ROWS_PER_MPI_PROCESS; j++)
				{
					for(int k = 0; k < COLUMNS_PER_MPI_PROCESS; k++)
					{
						temperatures_last[j][k] = all_temperatures[j-1][k];
					}
				}
			}
		}
	}
	else
	{
		// Receive my chunk.
		MPI_Recv(&temperatures_last[1][0], ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, MASTER_PROCESS_RANK, MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
	}
	// MPI_Scatter(all_temperatures, ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, &temperatures_last[1][0], ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, MASTER_PROCESS_RANK, MPI_COMM_WORLD);


	


	// #pragma acc data copyin(temperatures, temperatures_last)
	// {
		// Copy the temperatures into the current iteration temperature as well
		// #pragma acc kernels
		for(int i = 1; i <= ROWS_PER_MPI_PROCESS; i++)
		{
			for(int j = 0; j < COLUMNS_PER_MPI_PROCESS; j++)
			{
				temperatures[i][j] = temperatures_last[i][j];
			}
		}

		if(my_rank == MASTER_PROCESS_RANK)
		{
			printf("Data acquisition complete.\n");
		}

		/*
		// Wait for everybody to receive their part before we can start processing
		MPI_Barrier(MPI_COMM_WORLD);
		*/
		/////////////////////////////
		// TASK 2: DATA PROCESSING //
		/////////////////////////////

		int iteration_count = 0;
		/// Maximum temperature change observed across all MPI processes
		double global_temperature_change;

		/// Maximum temperature change for us
		double my_temperature_change; 
		
		/// The last snapshot made
		double snapshot[ROWS][COLUMNS];


		#pragma acc data copyin(temperatures, temperatures_last)
		while(total_time_so_far < MAX_TIME)
		{
			my_temperature_change = 0.0;

			// ////////////////////////////////////////
			// -- SUBTASK 1: EXCHANGE GHOST CELLS -- //
			// ////////////////////////////////////////
			 #pragma acc update host(temperatures[1:1][0:COLUMNS_PER_MPI_PROCESS], temperatures[ROWS_PER_MPI_PROCESS:1][0:COLUMNS_PER_MPI_PROCESS])  // only need to send this section of data back to host -> array[start_pos:length]
				// Send data to up neighbour for its ghost cells. If my up_neighbour_rank is MPI_PROC_NULL, this MPI_Ssend will do nothing.
				MPI_Ssend(&temperatures[1][0], COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, up_neighbour_rank, 0, MPI_COMM_WORLD);

				// Receive data from down neighbour to fill our ghost cells. If my down_neighbour_rank is MPI_PROC_NULL, this MPI_Recv will do nothing.
				MPI_Recv(&temperatures_last[ROWS_PER_MPI_PROCESS+1][0], COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, down_neighbour_rank, MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

				// Send data to down neighbour for its ghost cells. If my down_neighbour_rank is MPI_PROC_NULL, this MPI_Ssend will do nothing.
				MPI_Ssend(&temperatures[ROWS_PER_MPI_PROCESS][0], COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, down_neighbour_rank, 0, MPI_COMM_WORLD);

				// Receive data from up neighbour to fill our ghost cells. If my up_neighbour_rank is MPI_PROC_NULL, this MPI_Recv will do nothing.
				MPI_Recv(&temperatures_last[0][0], COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, up_neighbour_rank, MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			
				// ///////////// Using SendRecv
				// // Send data to up neighbour from down neighbour
				// MPI_Sendrecv(&temperatures[1][0], COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, up_neighbour_rank, 0, &temperatures_last[ROWS_PER_MPI_PROCESS+1][0], COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, down_neighbour_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				// // Send data to down neighbour from up neighbour
				// MPI_Sendrecv(&temperatures[ROWS_PER_MPI_PROCESS][0], COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, down_neighbour_rank, 0, &temperatures_last[0][0], COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, up_neighbour_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			// }
			#pragma acc update device(temperatures_last[ROWS_PER_MPI_PROCESS+1:1][0:COLUMNS_PER_MPI_PROCESS], temperatures_last[0:1][0:COLUMNS_PER_MPI_PROCESS]) 
			

			#pragma acc kernels async(1) 
			for(int i = 1; i <= ROWS_PER_MPI_PROCESS; i++)
			{
				if(temperatures[i][0] != MAX_TEMPERATURE) 		
				{
				temperatures[i][0] = (temperatures_last[i-1][0] +
										  temperatures_last[i+1][0] +
										  temperatures_last[i  ][1]) / 3.0;
				}
			}

			#pragma acc kernels loop independent collapse(2) async(2)
			for(int i = 1; i <= ROWS_PER_MPI_PROCESS; i++)
			{
				//#pragma acc loop vector
				for(int j = 1; j < COLUMNS_PER_MPI_PROCESS - 1; j++)
				{
					if(temperatures[i][j] != MAX_TEMPERATURE)
					{
						temperatures[i][j] = 0.25 * (temperatures_last[i-1][j  ] +
													 temperatures_last[i+1][j  ] +
													 temperatures_last[i  ][j-1] +
													 temperatures_last[i  ][j+1]);
					}
				}
			}

			#pragma acc kernels async(3)
			for(int i = 1; i <= ROWS_PER_MPI_PROCESS; i++)
			{
				if(temperatures[i][COLUMNS_PER_MPI_PROCESS - 1] != MAX_TEMPERATURE)
				{
				temperatures[i][COLUMNS_PER_MPI_PROCESS - 1] = (temperatures_last[i-1][COLUMNS_PER_MPI_PROCESS - 1] +
																    temperatures_last[i+1][COLUMNS_PER_MPI_PROCESS - 1] +
																    temperatures_last[i  ][COLUMNS_PER_MPI_PROCESS - 2]) / 3.0;
				}
			}
			

			///////////////////////////////////////////////////////
			// -- SUBTASK 3: CALCULATE MAX TEMPERATURE CHANGE -- //
			///////////////////////////////////////////////////////
			my_temperature_change = 0.0; 		
			#pragma acc parallel loop reduction(max:my_temperature_change) wait(1,2,3)
			for(int i = 1; i <= ROWS_PER_MPI_PROCESS; i++)
			{	
				#pragma acc loop reduction(max:my_temperature_change)
				for(int j = 0; j < COLUMNS_PER_MPI_PROCESS; j++)
				{
					my_temperature_change = fmax(fabs(temperatures[i][j] - temperatures_last[i][j]), my_temperature_change);
					temperatures_last[i][j] = temperatures[i][j];
				}
			}
		

			// Non-block (cheating) alternative 
			MPI_Request request1; 
			MPI_Iallreduce(&my_temperature_change, &global_temperature_change, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD, &request1); 		// Not doing linebreaking gave an error and it also makes the simulation slightly faster by a handful of iterations.

			// @ LUDOVIC CAPELLI: use this blocking MPI_Allreduce instead of the above, if you feel like its cheating. Remember to remove MPI_wait in if-statement L449. 
			// Reduce all the local max temps into global max temp using MPI_MAX op and then send back to all processes
			// MPI_Allreduce(&my_temperature_change, &global_temperature_change, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

			// MPI_Status status;
			// MPI_Wait(&request1, &status); 	// Blocking call

			///////////////////////////////////
			// -- SUBTASK 6: GET SNAPSHOT -- //
			///////////////////////////////////
			if(iteration_count % SNAPSHOT_INTERVAL == 0)
			{
				#pragma acc update host(temperatures[1:ROWS_PER_MPI_PROCESS][0:COLUMNS_PER_MPI_PROCESS]) // ignore the halos - no need to send those for the snapshot - saves 2 x COLUMNS_PER_MPI_PROCESS x num_procs data transfer 					
					if (my_rank == MASTER_PROCESS_RANK) {
						// Here is our (cheating) code MPI_wait for non-blocking Allreduce: 
						MPI_Status status;
						MPI_Wait(&request1, &status); 	// Blocking call
						printf("Iteration %d: %.18f\n", iteration_count, global_temperature_change);				
					}
					MPI_Gather(&temperatures[1][0], ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, snapshot, ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, MASTER_PROCESS_RANK, MPI_COMM_WORLD);
			}

			// Calculate the total time spent processing
			if(my_rank == MASTER_PROCESS_RANK)
			{
				total_time_so_far = MPI_Wtime() - start_time;
			}

			// Send total timer to everybody so they too can exit the loop if more than the allowed runtime has elapsed already
			MPI_Bcast(&total_time_so_far, 1, MPI_DOUBLE, MASTER_PROCESS_RANK, MPI_COMM_WORLD);

			// Update the iteration number
			iteration_count++;
		}
	// }
	///////////////////////////////////////////////
	//     ^                                     //
	//    / \                                    //
	//   / | \    CODE FROM HERE IS NOT TIMED    //
	//  /  o  \                                  //
	// /_______\                                 //
	///////////////////////////////////////////////

	/////////////////////////////////////////
	// -- FINALISATION 2: PRINT SUMMARY -- //
	/////////////////////////////////////////
	if(my_rank == MASTER_PROCESS_RANK)
	{
		printf("The program took %.2f seconds in total and executed %d iterations.\n", total_time_so_far, iteration_count);
	}

	MPI_Finalize();

	return EXIT_SUCCESS;
}
