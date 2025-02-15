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



		// Wait for everybody to receive their part before we can start processing
		MPI_Barrier(MPI_COMM_WORLD);
		
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

			// #pragma acc update host(temperatures, temperatures_last)
			// {
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
			#pragma acc update device(temperatures_last[ROWS_PER_MPI_PROCESS+1:1][0:COLUMNS_PER_MPI_PROCESS], temperatures_last[0:1][0:COLUMNS_PER_MPI_PROCESS]) // only need to update the device with this data -> array[start_pos:length]

			/////////////////////////////////////////////
			// -- SUBTASK 2: PROPAGATE TEMPERATURES -- //
			/////////////////////////////////////////////
			// #pragma acc kernels //loop gang(32), vector(16)    // 32 blocks of 16 threads in the first direction (rows)
			// for(int i = 1; i <= ROWS_PER_MPI_PROCESS; i++)
			// {
			// 	// if(temperatures[i][0] != MAX_TEMPERATURE)
			// 	// {
			// 	// 	temperatures[i][0] = (temperatures_last[i-1][0] +
			// 	// 						  temperatures_last[i+1][0] +
			// 	// 						  temperatures_last[i  ][1]) / 3.0;
			// 	// }

			// 	// Process all cells between the first and last columns excluded, which each has both left and right neighbours
			// 	// #pragma acc loop gang(16), vector(32)    // 16 blocks with 32 threads in the second direction (columns)
			// 	for(int j = 1; j < COLUMNS_PER_MPI_PROCESS - 1; j++)
			// 	{
			// 		// #pragma acc cache(temperatures_last[i-1:3][j], temperatures_last[i][j-1:3])
			// 		if(temperatures[i][j] != MAX_TEMPERATURE)
			// 		{
			// 			temperatures[i][j] = 0.25 * (temperatures_last[i-1][j  ] +
			// 										 temperatures_last[i+1][j  ] +
			// 										 temperatures_last[i  ][j-1] +
			// 										 temperatures_last[i  ][j+1]);
			// 		}
			// 	}

			// 	// if(temperatures[i][COLUMNS_PER_MPI_PROCESS - 1] != MAX_TEMPERATURE)
			// 	// {
			// 	// 	temperatures[i][COLUMNS_PER_MPI_PROCESS - 1] = (temperatures_last[i-1][COLUMNS_PER_MPI_PROCESS - 1] +
			// 	// 												    temperatures_last[i+1][COLUMNS_PER_MPI_PROCESS - 1] +
			// 	// 												    temperatures_last[i  ][COLUMNS_PER_MPI_PROCESS - 2]) / 3.0;
			// 	// }
			// }
			
			
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
			// %% KASPERS CHANGES TO THE ABOVE LOOP %%
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

			/*
			Firstly, the if-clauses are redundant. There is something wrong if the temperature stays at MAX_TEMPERATURE and then the thing will give incorrect results anyways. Omit and assume we are right--saves some iterations. 

			Maximum hierarchy of gangs, workers, and vectors
			Bridges uses Tesla V100s https://www.psc.edu/resources/bridges-2/user-guide-2/#system-configuration with 84 streaming multiprocessors (SM). 
			The max occupancy for a single SM is 32 thread-blocks and concurrently 64 warps per SM https://docs.nvidia.com/cuda/volta-tuning-guide/index.html#sm-independent-thread-scheduling
			Then, the best mapping of Openacc to that architecture is: 32 x 84 = 2688 blocks (gangs), 84 * 62 = 5208 warps (workers), and a warp is 32 threads on the accelerator. 
			The first loop (left boundary) has 1920 cells (excl. halos)
			Middle loop has 1920 * 15360 = 29,491,200 cells
			The last loop (right boundary) has 1920 cells 

			Each loop should be allocated: 
			0.17 blocks to each of the first and last loops (round up 1) 
			And for the middle loop allocate the remaining (2687)

			I am not sure what the syntax is like but I have tried: 
			acc kernels loop gang(2687) worker(5208) vector(166656) assumimng that it asks for number of blocks per device, warps per SM, and threads per SM (probably very incorrect--yields around 610 iterations)
			acc kernels loop gang(2687) worker(5208) vector(32) assumimng that it asks for number of blocks per device, warps per SM, and threads per warp (less incorrect--630 iterations)
			lets acc figure it out and achieve 651 iterations lol as in the current code. 
			If you know how this should be interpreted please let me know. 
			*/
			
			#pragma acc parallel loop  async(1)	device_type(acc_device_nvidia)// NUmber of vectors should be greater than the data size (1920 rows) -> 16 * 32 = 512. See page 11 https://www.openacc.org/sites/default/files/inline-files/API%20Guide%202.7.pdf for syntax of gang, worker, vector
			for(int i = 1; i <= ROWS_PER_MPI_PROCESS; i++)
			{	
				if(temperatures[i][0] != MAX_TEMPERATURE)
				{
					temperatures[i][0] = (temperatures_last[i-1][0] +
										  temperatures_last[i+1][0] +
										  temperatures_last[i  ][1]) / 3.0;
				}
			}

			#pragma acc parallel loop  independent collapse(2) async(2) device_type(acc_device_nvidia) //gang	// independent and collapse yield no difference--acc knows everything, why do I even need to give hints lol?
			for(int i = 1; i <= ROWS_PER_MPI_PROCESS; i++)
			{
				// #pragma acc loop vector(128)
				for(int j = 1; j < COLUMNS_PER_MPI_PROCESS - 1; j++)
				{
					if(temperatures[i][j] != MAX_TEMPERATURE)
					{
					// #pragma acc cache(temperatures_last[i-1:3][j], temperatures_last[i][j-1:3])
						temperatures[i][j] = 0.25 * (temperatures_last[i-1][j  ] +
													 temperatures_last[i+1][j  ] +
													 temperatures_last[i  ][j-1] +
													 temperatures_last[i  ][j+1]);
					}
				}
			}

			#pragma acc parallel loop  async(3) device_type(acc_device_nvidia)			
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
			#pragma acc parallel loop reduction(max:my_temperature_change) independent collapse(2) wait(1, 2, 3) device_type(acc_device_nvidia) //loop, gang(32), vector(16), 
			for(int i = 1; i <= ROWS_PER_MPI_PROCESS; i++)
			{	
				// #pragma acc loop //, gang(16), vector(32)
				for(int j = 0; j < COLUMNS_PER_MPI_PROCESS; j++)
				{
					my_temperature_change = fmax(fabs(temperatures[i][j] - temperatures_last[i][j]), my_temperature_change);
					temperatures_last[i][j] = temperatures[i][j];
				}
			}
			

			MPI_Request gather_request;
			if(iteration_count % SNAPSHOT_INTERVAL == 0)
			{	
				#pragma acc update host(temperatures[1:ROWS_PER_MPI_PROCESS][0:COLUMNS_PER_MPI_PROCESS]) wait(1, 2, 3) async(4) // ignore the halos - no need to send those for the snapshot - saves 2 x COLUMNS_PER_MPI_PROCESS x num_procs data transfer 
				MPI_Igather(&temperatures[1][0], ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, snapshot, ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, MASTER_PROCESS_RANK, MPI_COMM_WORLD, &gather_request);
			}
			//////////////////////////////////////////////////////////
			// -- SUBTASK 4: FIND MAX TEMPERATURE CHANGE OVERALL -- //
			//////////////////////////////////////////////////////////
			// if(my_rank != MASTER_PROCESS_RANK)
			// {
			// 	// Send my temperature delta to the master MPI process
			// 	MPI_Ssend(&my_temperature_change, 1, MPI_DOUBLE, MASTER_PROCESS_RANK, 0, MPI_COMM_WORLD);
				
			// 	// Receive the total delta calculated by the MPI process based on all MPI processes delta
			// 	MPI_Recv(&global_temperature_change, 1, MPI_DOUBLE, MASTER_PROCESS_RANK, MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			// }
			// else
			// {
			// 	// Initialise the temperature change to mine
			// 	global_temperature_change = my_temperature_change;

			// 	// Pick highest temperature change observed
			// 	for(int j = 0; j < comm_size; j++)
			// 	{
			// 		if(j != my_rank)
			// 		{
			// 			double subtotal;
			// 			MPI_Recv(&subtotal, 1, MPI_DOUBLE, j, MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			// 			if(subtotal > global_temperature_change)
			// 			{
			// 				global_temperature_change = subtotal;
			// 			}
			// 		}
			// 	}

			// 	// Send delta back to all MPI processes
			// 	for(int j = 0; j < comm_size; j++)
			// 	{
			// 		if(j != my_rank)
			// 		{
			// 			MPI_Ssend(&global_temperature_change, 1, MPI_DOUBLE, j, 0, MPI_COMM_WORLD);
			// 		}
			// 	}
			// }
			// Reduce all the local max temps into global max temp using MPI_MAX op and then send back to all processes
			// MPI_Allreduce(&my_temperature_change, &global_temperature_change, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

			// %%%%%%%%%%%%%%%%%%%%%
			// %% KASPERS CHANGES %%
			// %%%%%%%%%%%%%%%%%%%%%
			// Non-block alternative (gives a dozen iterations improvement): 
			MPI_Request request1; 
			MPI_Iallreduce(&my_temperature_change, &global_temperature_change, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD, &request1); 		// Not doing linebreaking gave an error and it also makes the simulation slightly faster by a handful of iterations.

			//////////////////////////////////////////////////
			// -- SUBTASK 5: UPDATE LAST ITERATION ARRAY -- //
			//////////////////////////////////////////////////
			// #pragma acc kernels
			// for(int i = 1; i <= ROWS_PER_MPI_PROCESS; i++)
			// {
			// 	for(int j = 0; j < COLUMNS_PER_MPI_PROCESS; j++)
			// 	{
			// 		temperatures_last[i][j] = temperatures[i][j];
			// 	}
			// }

			// MPI_Status status;
			// MPI_Wait(&request1, 
			// &status); 	// Blocking call
			
			MPI_Wait(&request1, MPI_STATUS_IGNORE); 	// Blocking call

			///////////////////////////////////
			// -- SUBTASK 6: GET SNAPSHOT -- //
			///////////////////////////////////
			if(iteration_count % SNAPSHOT_INTERVAL == 0)
			{
				// #pragma acc update host(temperatures[1:ROWS_PER_MPI_PROCESS][0:COLUMNS_PER_MPI_PROCESS]) // ignore the halos - no need to send those for the snapshot - saves 2 x COLUMNS_PER_MPI_PROCESS x num_procs data transfer 
				
					// if(my_rank == MASTER_PROCESS_RANK)
					// {
					// 	for(int j = 0; j < comm_size; j++)
					// 	{
					// 		if(j == my_rank)
					// 		{
					// 			// Copy locally my own temperature array in the global one
					// 			for(int k = 0; k < ROWS_PER_MPI_PROCESS; k++)
					// 			{
					// 				for(int l = 0; l < COLUMNS_PER_MPI_PROCESS; l++)
					// 				{
					// 					snapshot[j * ROWS_PER_MPI_PROCESS + k][l] = temperatures[k + 1][l];
					// 				}
					// 			}
					// 		}
					// 		else
					// 		{
					// 			MPI_Recv(&snapshot[j * ROWS_PER_MPI_PROCESS][0], ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, j, MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
					// 		}
					// 	}

					// 	printf("Iteration %d: %.18f\n", iteration_count, global_temperature_change);
					// }
					// else
					// {
					// 	// Send my array to the master MPI process
					// 	MPI_Ssend(&temperatures[1][0], ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, MASTER_PROCESS_RANK, 0, MPI_COMM_WORLD); 
					// }
			

					if (my_rank == MASTER_PROCESS_RANK) {
						MPI_Wait(&gather_request, MPI_STATUS_IGNORE);
						// Wait there to gather the snapshot
						printf("Iteration %d: %.18f\n", iteration_count, global_temperature_change);				
					}
					// MPI_Gather(&temperatures[1][0], ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, snapshot, ROWS_PER_MPI_PROCESS * COLUMNS_PER_MPI_PROCESS, MPI_DOUBLE, MASTER_PROCESS_RANK, MPI_COMM_WORLD);
				
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
