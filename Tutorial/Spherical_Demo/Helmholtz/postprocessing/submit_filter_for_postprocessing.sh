#!/bin/bash
#SBATCH --output=sim-%j.log
#SBATCH --error=sim-%j.err
#SBATCH --time=00-01:00:00                              # time (DD-HH:MM:SS)
#SBATCH --ntasks=1                                      # can only use one MPI process
#SBATCH --cpus-per-task=6                               # but we can use multiple openmp threads
#SBATCH --mem-per-cpu=3G
#SBATCH --job-name="Coarse-grain : Postprocess Demo"

export OMP_NUM_THREADS=${SLURM_CPUS_PER_TASK}  # SLURM_CPUS_PER_TASK is the number of Openmp threads per MPI process
export KMP_AFFINITY="compact"
export I_MPI_PIN_DOMAIN="auto"

FILTER_SCALES="
1.e4 1.29e4 1.67e4 2.15e4 2.78e4 3.59e4 4.64e4 5.99e4 7.74e4 
1.e5 1.29e5 1.67e5 2.15e5 2.78e5 3.59e5 4.64e5 5.99e5 7.74e5 
1.e6 1.29e6 1.67e6 2.15e6 2.78e6 3.59e6 4.64e6 5.99e6 7.74e6 
1.e7" 

REGIONS_FILE="region_definitions.nc"

mpirun -n ${SLURM_NTASKS} ./coarse_grain_helmholtz.x \
        --Helmholtz_input_file ../project/projection_ui.nc \
        --velocity_input_file ../velocity_sample.nc \
        --tor_field Psi \
        --pot_field Phi \
        --vel_field uo \
        --region_definitions_file ${REGIONS_FILE} \
        --filter_scales "${FILTER_SCALES}"

