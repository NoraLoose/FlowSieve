#include <math.h>
#include <algorithm>
#include <vector>
#include <omp.h>
#include <mpi.h>
#include "../../functions.hpp"
#include "../../netcdf_io.hpp"
#include "../../constants.hpp"
#include "../../postprocess.hpp"
#include "../../preprocess.hpp"

/*!
 * \brief Main filtering driver for Helmholtz decomposed data
 *
 * This function is the main filtering driver. It sets up the appropriate
 * loop sequences, calls the other funcations (velocity conversions), and
 * calls the IO functionality.
 *
 * @param[in]   source_data     dataset class instance containing data (Psi, Phi, etc)
 * @param[in]   scales          scales at which to filter the data
 * @param[in]   comm            MPI communicator (default MPI_COMM_WORLD)
 *
 */
void filtering_helmholtz(
        const dataset & source_data,
        const std::vector<double> & scales,
        const MPI_Comm comm
        ) {

    // Get dimension sizes
    const int   Nscales = scales.size(),
                Ntime   = source_data.Ntime,    // this is the MPI-local Ntime, not the full Ntime
                Ndepth  = source_data.Ndepth,   // this is the MPI-local Ndepth, not the full Ndepth
                Nlat    = source_data.Nlat,
                Nlon    = source_data.Nlon;
    const unsigned int num_pts = Ntime * Ndepth * Nlat * Nlon;

    const std::vector<double> zero_vector( num_pts, 0. );

    // Create some tidy names for variables
    const std::vector<double>   &time       = source_data.time,
                                &depth      = source_data.depth,
                                &latitude   = source_data.latitude,
                                &longitude  = source_data.longitude,
                                &dAreas     = source_data.areas;

    const std::vector<double>   &F_potential    = source_data.variables.at("F_potential"),
                                &F_toroidal     = source_data.variables.at("F_toroidal");

    const std::vector<double>   &uiuj_F_r   = ( constants::COMP_PI_HELMHOLTZ ) ? source_data.variables.at("uiuj_F_r")   : zero_vector,
                                &uiuj_F_Phi = ( constants::COMP_PI_HELMHOLTZ ) ? source_data.variables.at("uiuj_F_Phi") : zero_vector,
                                &uiuj_F_Psi = ( constants::COMP_PI_HELMHOLTZ ) ? source_data.variables.at("uiuj_F_Psi") : zero_vector;

    const std::vector<bool> &mask = source_data.mask;

    const std::vector<int>  &myCounts = source_data.myCounts,
                            &myStarts = source_data.myStarts;

    // Get some MPI info
    int wRank, wSize;
    MPI_Comm_rank( comm, &wRank );
    MPI_Comm_size( comm, &wSize );

    #if DEBUG >= 2
    if (wRank == 0) { fprintf(stdout, "\nEntered filtering_helmholtz\n\n"); }
    #endif

    // If we've passed the DO_TIMING flag, then create some timing vars
    Timing_Records timing_records;
    double clock_on;

    #if DEBUG >= 1
    if (wRank == 0) { fprintf( stdout, "\nPreparing to apply %d filters to data with (MPI-local) sizes (%'d - %'d - %'d - %'d) \n", Nscales, Ntime, Ndepth, Nlat, Nlon ); }
    #endif

    const int OMP_chunksize = get_omp_chunksize(Nlat,Nlon);

    char fname [50];
    
    const int ndims = 4;
    size_t starts[ndims] = { size_t(myStarts.at(0)), size_t(myStarts.at(1)), size_t(myStarts.at(2)), size_t(myStarts.at(3)) };
    size_t counts[ndims] = { size_t(Ntime),          size_t(Ndepth),         size_t(Nlat),           size_t(Nlon)           };
    size_t index;
    std::vector<std::string> vars_to_write;

    int LAT_lb, LAT_ub;

    std::vector<double> local_kernel(Nlat * Nlon, 0.);

    std::vector<double> null_vector(0);

    #if DEBUG >= 2
    if (wRank == 0) { fprintf(stdout, "\nInitializing storage arrays.\n"); }
    #endif
    std::vector<double> 
        // Arrays to store filtered Phi and Psi fields (potential, pseudo-potential)
        coarse_F_tor(   num_pts, 0. ),
        coarse_F_pot(   num_pts, 0. ),

        coarse_uiuj_F_r(   num_pts, 0. ),
        coarse_uiuj_F_Phi( num_pts, 0. ),
        coarse_uiuj_F_Psi( num_pts, 0. ),

        // Original KE
        KE_tor_orig(    num_pts, 0. ),
        KE_pot_orig(    num_pts, 0. ),
        KE_tot_orig(    num_pts, 0. ),

        // Coarse KE (computed from velocities)
        KE_tor_coarse(  num_pts, 0. ),
        KE_pot_coarse(  num_pts, 0. ),
        KE_tot_coarse(  num_pts, 0. ),

        // Fine KE ( tau(uu) = bar(uu) - bar(u)bar(u) )
        KE_tor_fine(    num_pts, 0. ),
        KE_pot_fine(    num_pts, 0. ),
        KE_tot_fine(    num_pts, 0. ),

        // Fine KE modified ( uu - bar(u)bar(u) )
        KE_tor_fine_mod(    num_pts, 0. ),
        KE_pot_fine_mod(    num_pts, 0. ),
        KE_tot_fine_mod(    num_pts, 0. ),

        // Filtered KE (used to compute fine KE)
        KE_tor_filt(    num_pts, 0. ),
        KE_pot_filt(    num_pts, 0. ),
        KE_tot_filt(    num_pts, 0. ),

        // Energy transport
        div_J_tor( num_pts, 0. ),
        div_J_pot( num_pts, 0. ),
        div_J_tot( num_pts, 0. ),

        // Enstrophy
        Enst_tor( num_pts, 0. ),
        Enst_pot( num_pts, 0. ),
        Enst_tot( num_pts, 0. ),

        // Velocity divergences
        div_tor(        num_pts, 0. ),
        div_pot(        num_pts, 0. ),
        div_tot(        num_pts, 0. ),

        // Cartensian velocities
        u_x_tor( num_pts, 0. ),
        u_y_tor( num_pts, 0. ),
        u_z_tor( num_pts, 0. ),

        u_x_pot( num_pts, 0. ),
        u_y_pot( num_pts, 0. ),
        u_z_pot( num_pts, 0. ),

        u_x_tot( num_pts, 0. ),
        u_y_tot( num_pts, 0. ),
        u_z_tot( num_pts, 0. ),

        u_x_coarse( num_pts, 0. ),
        u_y_coarse( num_pts, 0. ),
        u_z_coarse( num_pts, 0. ),

        //
        //// Diadic (Cartesian) velocity components
        //

        // tor
        ux_ux_tor( num_pts, 0. ),
        ux_uy_tor( num_pts, 0. ),
        ux_uz_tor( num_pts, 0. ),
        uy_uy_tor( num_pts, 0. ),
        uy_uz_tor( num_pts, 0. ),
        uz_uz_tor( num_pts, 0. ),

        vort_ux_tor( num_pts, 0. ),
        vort_uy_tor( num_pts, 0. ),
        vort_uz_tor( num_pts, 0. ),

        // pot
        ux_ux_pot( num_pts, 0. ),
        ux_uy_pot( num_pts, 0. ),
        ux_uz_pot( num_pts, 0. ),
        uy_uy_pot( num_pts, 0. ),
        uy_uz_pot( num_pts, 0. ),
        uz_uz_pot( num_pts, 0. ),

        vort_ux_pot( num_pts, 0. ),
        vort_uy_pot( num_pts, 0. ),
        vort_uz_pot( num_pts, 0. ),

        // tot
        ux_ux_tot( num_pts, 0. ),
        ux_uy_tot( num_pts, 0. ),
        ux_uz_tot( num_pts, 0. ),
        uy_uy_tot( num_pts, 0. ),
        uy_uz_tot( num_pts, 0. ),
        uz_uz_tot( num_pts, 0. ),

        vort_ux_tot( num_pts, 0. ),
        vort_uy_tot( num_pts, 0. ),
        vort_uz_tot( num_pts, 0. ),

        //
        //// Spherical velocity components
        //

        // Spherical - radial velocities (just set to zero)
        u_r_zero( num_pts, 0.),

        // Spherical - zonal velocities
        u_lon_tor( num_pts, 0. ),
        u_lon_pot( num_pts, 0. ),
        u_lon_tot( num_pts, 0. ),

        // Spherical - meridional velocities
        u_lat_tor( num_pts, 0. ),
        u_lat_pot( num_pts, 0. ),
        u_lat_tot( num_pts, 0. ),

        // Spherical - dyadix products
        ulon_ulon( num_pts, 0. ),
        ulon_ulat( num_pts, 0. ),
        ulat_ulat( num_pts, 0. ),

        // Vorticity (only r component)
        vort_tor_r( num_pts, 0. ),
        vort_pot_r( num_pts, 0. ),
        vort_tot_r( num_pts, 0. ),

        // Full vorticity components
        full_vort_tor_r( num_pts, 0. ),
        full_vort_pot_r( num_pts, 0. ),
        full_vort_tot_r( num_pts, 0. ),

        // Okubo-Weiss values
        OkuboWeiss_tor( num_pts, 0. ),
        OkuboWeiss_pot( num_pts, 0. ),
        OkuboWeiss_tot( num_pts, 0. ),

        // Pi
        Pi_tor(  num_pts, 0. ),
        Pi_pot(  num_pts, 0. ),
        Pi_tot(  num_pts, 0. ),
        Pi_Helm( num_pts, 0. ),

        // Pi - shifted derivatives
        Pi2_tor(  num_pts, 0. ),
        Pi2_pot(  num_pts, 0. ),
        Pi2_tot(  num_pts, 0. ),

        // Z ( enstrophy cascade )
        Z_tor( num_pts, 0. ),
        Z_pot( num_pts, 0. ),
        Z_tot( num_pts, 0. );

    //
    //// Compute original (unfiltered) KE
    //
     
    #if DEBUG >= 2
    if (wRank == 0) { fprintf(stdout, "\nExtracting velocities from Phi and Psi\n"); }
    #endif
    // Get pot and tor velocities
    toroidal_vel_from_F(  u_lon_tor, u_lat_tor, F_toroidal,  longitude, latitude, Ntime, Ndepth, Nlat, Nlon, mask );
    potential_vel_from_F( u_lon_pot, u_lat_pot, F_potential, longitude, latitude, Ntime, Ndepth, Nlat, Nlon, mask );

    #if DEBUG >= 2
    if (wRank == 0) { fprintf(stdout, "\nComputing KE of unfiltered velocities\n"); }
    #endif
    #pragma omp parallel \
    default( none ) \
    shared( KE_tor_orig, KE_pot_orig, KE_tot_orig, mask, \
            u_lon_tor, u_lat_tor, u_lon_pot, u_lat_pot, u_lon_tot, u_lat_tot) \
    private( index )
    {
        #pragma omp for collapse(1) schedule(dynamic, OMP_chunksize)
        for (index = 0; index < u_lon_tor.size(); ++index) {
            u_lon_tot.at(index) = u_lon_tor.at(index) + u_lon_pot.at(index);
            u_lat_tot.at(index) = u_lat_tor.at(index) + u_lat_pot.at(index);
            if ( mask.at(index) ) { 
                KE_tor_orig.at(index) = 0.5 * constants::rho0 * ( pow(u_lon_tor.at(index), 2.) + pow(u_lat_tor.at(index), 2.) );
                KE_pot_orig.at(index) = 0.5 * constants::rho0 * ( pow(u_lon_pot.at(index), 2.) + pow(u_lat_pot.at(index), 2.) );
                KE_tot_orig.at(index) = 0.5 * constants::rho0 * ( pow(u_lon_tot.at(index), 2.) + pow(u_lat_tot.at(index), 2.) );
            } else {
                KE_tor_orig.at(index) = 0.;
                KE_pot_orig.at(index) = 0.;
                KE_tot_orig.at(index) = 0.;
            }
        }
    }

    // Get vorticities
    compute_vorticity( full_vort_tor_r, null_vector, null_vector, null_vector, null_vector,
                u_r_zero, u_lon_tor, u_lat_tor, Ntime, Ndepth, Nlat, Nlon, longitude, latitude, mask);
    compute_vorticity( full_vort_pot_r, null_vector, null_vector, null_vector, null_vector,
                u_r_zero, u_lon_pot, u_lat_pot, Ntime, Ndepth, Nlat, Nlon, longitude, latitude, mask);
    compute_vorticity( full_vort_tot_r, null_vector, null_vector, null_vector, null_vector,
                u_r_zero, u_lon_tot, u_lat_tot, Ntime, Ndepth, Nlat, Nlon, longitude, latitude, mask);

    #if DEBUG >= 2
    if (wRank == 0) { fprintf(stdout, "\nGetting Cartesian velocity components\n"); }
    #endif
    // Get Cartesian velocities, will need them for Pi
    vel_Spher_to_Cart( u_x_tor, u_y_tor, u_z_tor, u_r_zero, u_lon_tor, u_lat_tor, source_data );
    vel_Spher_to_Cart( u_x_pot, u_y_pot, u_z_pot, u_r_zero, u_lon_pot, u_lat_pot, source_data );
    vel_Spher_to_Cart( u_x_tot, u_y_tot, u_z_tot, u_r_zero, u_lon_tot, u_lat_tot, source_data );

    #if DEBUG >= 2
    if (wRank == 0) { fprintf(stdout, "\nFlagging variables for output\n"); }
    #endif
    if (not(constants::NO_FULL_OUTPUTS)) {
        //
        // These variables are output unless full outputs are turned off
        // 

        vars_to_write.push_back("coarse_F_tor");
        vars_to_write.push_back("coarse_F_pot");

        if ( constants::COMP_PI_HELMHOLTZ ) {
            vars_to_write.push_back("coarse_uiuj_F_r");
            vars_to_write.push_back("coarse_uiuj_F_Phi");
            vars_to_write.push_back("coarse_uiuj_F_Psi");
        }

        vars_to_write.push_back("u_lon_tor");
        vars_to_write.push_back("u_lat_tor");

        vars_to_write.push_back("u_lon_pot");
        vars_to_write.push_back("u_lat_pot");

        vars_to_write.push_back("KE_tor_fine");
        vars_to_write.push_back("KE_pot_fine");
        vars_to_write.push_back("KE_tot_fine");

        vars_to_write.push_back("Pi_tor");
        vars_to_write.push_back("Pi_pot");
        vars_to_write.push_back("Pi_tot");
        if ( constants::COMP_PI_HELMHOLTZ ) {
            vars_to_write.push_back("Pi_Helm");
        }

        vars_to_write.push_back("Pi2_tor");
        vars_to_write.push_back("Pi2_pot");
        vars_to_write.push_back("Pi2_tot");

        vars_to_write.push_back("Z_tor");
        vars_to_write.push_back("Z_pot");
        vars_to_write.push_back("Z_tot");
    }

    if (not(constants::MINIMAL_OUTPUT)) {
        //
        // These outputs are only included if not set to minimal outputs
        //

        vars_to_write.push_back("KE_tor_fine_mod");
        vars_to_write.push_back("KE_pot_fine_mod");
        vars_to_write.push_back("KE_tot_fine_mod");

        vars_to_write.push_back("div_tor");
        vars_to_write.push_back("div_pot");
        vars_to_write.push_back("div_tot");

        if (constants::DO_OKUBOWEISS_ANALYSIS) {
            vars_to_write.push_back("OkuboWeiss_tor");
            vars_to_write.push_back("OkuboWeiss_pot");
            vars_to_write.push_back("OkuboWeiss_tot");
        }

        vars_to_write.push_back("KE_tor_filt");
        vars_to_write.push_back("KE_pot_filt");
        vars_to_write.push_back("KE_tot_filt");

        vars_to_write.push_back("Enstrophy_tor");
        vars_to_write.push_back("Enstrophy_pot");
        vars_to_write.push_back("Enstrophy_tot");

        vars_to_write.push_back("vort_r_tor");
        vars_to_write.push_back("vort_r_pot");
        vars_to_write.push_back("vort_r_tot");

        if ( constants::COMP_PI_HELMHOLTZ ) {
            vars_to_write.push_back("coarse_uu");
            vars_to_write.push_back("coarse_uv");
            vars_to_write.push_back("coarse_vv");
        }
    }

    // Compute the kernal alpha value (for baroclinic transfers)
    const double kern_alpha = kernel_alpha();

    // Now prepare to filter
    double scale;
    int Itime, Idepth, Ilat, Ilon, tid;

    int perc_base = 5;
    int perc, perc_count=0;

    //
    //// Set up filtering vectors
    //
    std::vector<double*> filtered_vals;
    std::vector<bool> filt_use_mask;
    std::vector<const std::vector<double>*> filter_fields;

    double F_pot_tmp;
    filter_fields.push_back(&F_potential);
    filt_use_mask.push_back(false);

    double F_tor_tmp;
    filter_fields.push_back(&F_toroidal);
    filt_use_mask.push_back(false);

    double uiuj_F_r_tmp, uiuj_F_Phi_tmp, uiuj_F_Psi_tmp;
    if ( constants::COMP_PI_HELMHOLTZ ) {
        filter_fields.push_back(&uiuj_F_r);
        filt_use_mask.push_back(false);

        filter_fields.push_back(&uiuj_F_Psi);
        filt_use_mask.push_back(false);

        filter_fields.push_back(&uiuj_F_Phi);
        filt_use_mask.push_back(false);
    }


    double uxux_tmp, uxuy_tmp, uxuz_tmp, uyuy_tmp, uyuz_tmp, uzuz_tmp, vort_ux_tmp, vort_uy_tmp, vort_uz_tmp;

    //
    //// Set up post-processing variables
    //
    #if DEBUG >= 2
    if (wRank == 0) { fprintf(stdout, "\nFlagging variables for post-processing\n"); }
    #endif
    std::vector<const std::vector<double>*> postprocess_fields_tor, postprocess_fields_pot, postprocess_fields_tot;
    std::vector<std::string> postprocess_names;

    postprocess_names.push_back( "F" );
    postprocess_fields_tor.push_back( &coarse_F_tor );
    postprocess_fields_pot.push_back( &coarse_F_pot );
    postprocess_fields_tot.push_back( &u_r_zero     );

    postprocess_names.push_back( "coarse_KE" );
    postprocess_fields_tor.push_back( &KE_tor_coarse );
    postprocess_fields_pot.push_back( &KE_pot_coarse );
    postprocess_fields_tot.push_back( &KE_tot_coarse );

    postprocess_names.push_back( "fine_KE" );
    postprocess_fields_tor.push_back( &KE_tor_fine );
    postprocess_fields_pot.push_back( &KE_pot_fine );
    postprocess_fields_tot.push_back( &KE_tot_fine );

    postprocess_names.push_back( "Fine_KE_mod" );
    postprocess_fields_tor.push_back( &KE_tor_fine_mod );
    postprocess_fields_pot.push_back( &KE_pot_fine_mod );
    postprocess_fields_tot.push_back( &KE_tot_fine_mod );

    postprocess_names.push_back( "div_J_transport" );
    postprocess_fields_tor.push_back( &div_J_tor );
    postprocess_fields_pot.push_back( &div_J_pot );
    postprocess_fields_tot.push_back( &div_J_tot );

    postprocess_names.push_back( "enstrophy" );
    postprocess_fields_tor.push_back( &Enst_tor );
    postprocess_fields_pot.push_back( &Enst_pot );
    postprocess_fields_tot.push_back( &Enst_tot );

    postprocess_names.push_back( "u_lon" );
    postprocess_fields_tor.push_back( &u_lon_tor );
    postprocess_fields_pot.push_back( &u_lon_pot );
    postprocess_fields_tot.push_back( &u_lon_tot );

    postprocess_names.push_back( "u_lat" );
    postprocess_fields_tor.push_back( &u_lat_tor );
    postprocess_fields_pot.push_back( &u_lat_pot );
    postprocess_fields_tot.push_back( &u_lat_tot );

    if (constants::DO_OKUBOWEISS_ANALYSIS) {
        postprocess_names.push_back( "OkuboWeiss" );
        postprocess_fields_tor.push_back( &OkuboWeiss_tor );
        postprocess_fields_pot.push_back( &OkuboWeiss_pot );
        postprocess_fields_tot.push_back( &OkuboWeiss_tot );
    }

    postprocess_names.push_back( "Pi" );
    postprocess_fields_tor.push_back( &Pi_tor );
    postprocess_fields_pot.push_back( &Pi_pot );
    postprocess_fields_tot.push_back( &Pi_tot );

    postprocess_names.push_back( "Pi2" );
    postprocess_fields_tor.push_back( &Pi2_tor );
    postprocess_fields_pot.push_back( &Pi2_pot );
    postprocess_fields_tot.push_back( &Pi2_tot );

    if ( constants::COMP_PI_HELMHOLTZ ) {
        postprocess_names.push_back( "Pi_Helm" );
        postprocess_fields_tor.push_back( &Pi_Helm );
        postprocess_fields_pot.push_back( &Pi_Helm );
        postprocess_fields_tot.push_back( &Pi_Helm );
    }

    postprocess_names.push_back( "Z" );
    postprocess_fields_tor.push_back( &Z_tor );
    postprocess_fields_pot.push_back( &Z_pot );
    postprocess_fields_tot.push_back( &Z_tot );

    postprocess_names.push_back( "velocity_divergence" );
    postprocess_fields_tor.push_back( &div_tor );
    postprocess_fields_pot.push_back( &div_pot );
    postprocess_fields_tot.push_back( &div_tot );

    //
    //// Begin the main filtering loop
    //
    #if DEBUG>=1
    if (wRank == 0) { fprintf(stdout, "\nBeginning main filtering loop.\n\n"); }
    #endif
    for (int Iscale = 0; Iscale < Nscales; Iscale++) {

        // Rest our timing records
        timing_records.reset();

        // Create the output file
        snprintf(fname, 50, "filter_%.6gkm.nc", scales.at(Iscale)/1e3);
        if (not(constants::NO_FULL_OUTPUTS)) {
            initialize_output_file( source_data, vars_to_write, fname, scales.at(Iscale));

            // Add some attributes to the file
            add_attr_to_file("kernel_alpha", kern_alpha, fname);
        }

        #if DEBUG >= 0
        if (wRank == 0) { 
            fprintf(stdout, "\nScale %d of %d (%.5g km)\n", 
                Iscale+1, Nscales, scales.at(Iscale)/1e3); 
        }
        #endif

        scale = scales.at(Iscale);
        perc  = perc_base;

        #if DEBUG >= 1
        if (wRank == 0) { fprintf(stdout, "  filtering: "); }
        fflush(stdout);
        #endif

        #pragma omp parallel \
        default(none) \
        shared( source_data, mask, stdout, perc_base, \
                filter_fields, filt_use_mask, \
                timing_records, clock_on, \
                longitude, latitude, dAreas, scale, \
                F_potential, F_toroidal, coarse_F_tor, coarse_F_pot, \
                full_vort_tor_r, full_vort_pot_r, full_vort_tot_r, \
                u_x_tor, u_y_tor, u_z_tor, u_x_pot, u_y_pot, u_z_pot, u_x_tot, u_y_tot, u_z_tot, \
                ux_ux_tor, ux_uy_tor, ux_uz_tor, uy_uy_tor, uy_uz_tor, uz_uz_tor,\
                ux_ux_pot, ux_uy_pot, ux_uz_pot, uy_uy_pot, uy_uz_pot, uz_uz_pot,\
                ux_ux_tot, ux_uy_tot, ux_uz_tot, uy_uy_tot, uy_uz_tot, uz_uz_tot,\
                vort_ux_tor, vort_uy_tor, vort_uz_tor, \
                vort_ux_pot, vort_uy_pot, vort_uz_pot, \
                vort_ux_tot, vort_uy_tot, vort_uz_tot, \
                KE_tor_filt, KE_pot_filt, KE_tot_filt, \
                uiuj_F_r, uiuj_F_Phi, uiuj_F_Psi, coarse_uiuj_F_r, coarse_uiuj_F_Phi, coarse_uiuj_F_Psi\
                ) \
        private(Itime, Idepth, Ilat, Ilon, index, \
                F_tor_tmp, F_pot_tmp, uxux_tmp, uxuy_tmp, uxuz_tmp, uyuy_tmp, uyuz_tmp, uzuz_tmp, \
                vort_ux_tmp, vort_uy_tmp, vort_uz_tmp, LAT_lb, LAT_ub, tid, filtered_vals, \
                uiuj_F_r_tmp, uiuj_F_Phi_tmp, uiuj_F_Psi_tmp ) \
        firstprivate(perc, wRank, local_kernel, perc_count)
        {

            filtered_vals.clear();

            filtered_vals.push_back(&F_pot_tmp);
            filtered_vals.push_back(&F_tor_tmp);

            if ( constants::COMP_PI_HELMHOLTZ ) {
                filtered_vals.push_back(&uiuj_F_r_tmp);
                filtered_vals.push_back(&uiuj_F_Psi_tmp);
                filtered_vals.push_back(&uiuj_F_Phi_tmp);
            }

            tid = omp_get_thread_num();

            #pragma omp for collapse(1) schedule(dynamic)
            for (Ilat = 0; Ilat < Nlat; Ilat++) {

                get_lat_bounds(LAT_lb, LAT_ub, latitude,  Ilat, scale); 
                #if DEBUG >= 3
                if (wRank == 0) { fprintf(stdout, "Ilat (%d) has loop bounds %d and %d.\n", Ilat, LAT_lb, LAT_ub); }
                #endif

                // If our longitude grid is uniform, and spans the full periodic domain,
                // then we can just compute it once and translate it at each lon index
                if ( (constants::PERIODIC_X) and (constants::UNIFORM_LON_GRID) and (constants::FULL_LON_SPAN) ) {
                    if ( (constants::DO_TIMING) and (tid == 0) ) { clock_on = MPI_Wtime(); }
                    std::fill(local_kernel.begin(), local_kernel.end(), 0);
                    compute_local_kernel( local_kernel, scale, source_data, Ilat, 0, LAT_lb, LAT_ub );
                    if ( (constants::DO_TIMING) and (tid == 0) ) { timing_records.add_to_record(MPI_Wtime() - clock_on, "kernel_precomputation_outer"); }
                }

                for (Ilon = 0; Ilon < Nlon; Ilon++) {

                    #if DEBUG >= 0
                    if ( (tid == 0) and (wRank == 0) ) {
                        // Every perc_base percent, print a dot, but only the first thread
                        if ( ((double)(Ilat*Nlon + Ilon + 1) / (Nlon*Nlat)) * 100 >= perc ) {
                            perc_count++;
                            if (perc_count % 5 == 0) { fprintf(stdout, "|"); }
                            else                     { fprintf(stdout, "."); }
                            fflush(stdout);
                            perc += perc_base;
                        }
                    }
                    #endif

                    if ( not( (constants::PERIODIC_X) and (constants::UNIFORM_LON_GRID) and (constants::FULL_LON_SPAN) ) ) {
                        if ( (constants::DO_TIMING) and (tid == 0) ) { clock_on = MPI_Wtime(); }
                        // If we couldn't precompute the kernel earlier, then do it now
                        std::fill(local_kernel.begin(), local_kernel.end(), 0);
                        compute_local_kernel( local_kernel, scale, source_data, Ilat, Ilon, LAT_lb, LAT_ub );
                        if ( constants::DO_TIMING and (tid == 0) ) { timing_records.add_to_record(MPI_Wtime() - clock_on, "kernel_precomputation_inner"); }
                    }

                    for (Itime = 0; Itime < Ntime; Itime++) {
                        for (Idepth = 0; Idepth < Ndepth; Idepth++) {

                            // Convert our four-index to a one-index
                            index = Index(Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon);

                            // The F_tor and F_pot fields exist over land from the projection
                            //     procedure, so do those filtering operations on land as well.
                            // The other stuff (KE, etc), will only be done on water cells

                            // Apply the filter at the point
                            if ( (constants::DO_TIMING) and (tid == 0) ) { clock_on = MPI_Wtime(); }
                            apply_filter_at_point(  filtered_vals, filter_fields, source_data, Itime, Idepth, Ilat, Ilon, 
                                                    LAT_lb, LAT_ub, scale, filt_use_mask, local_kernel );
                            if ( (constants::DO_TIMING) and (tid == 0) ) { timing_records.add_to_record(MPI_Wtime() - clock_on, "filter_at_point"); }

                            // Store the filtered values in the appropriate arrays
                            coarse_F_pot.at(index) = F_pot_tmp;
                            coarse_F_tor.at(index) = F_tor_tmp;

                            if ( constants::COMP_PI_HELMHOLTZ ) {
                                coarse_uiuj_F_r.at(  index) = uiuj_F_r_tmp;
                                coarse_uiuj_F_Phi.at(index) = uiuj_F_Phi_tmp;
                                if ( ( uiuj_F_Phi_tmp == 0 ) and ( wRank == 0 ) ) {
                                    fprintf( stdout, " bar(F_phi[%'d,%'d]) = 0 (loc val is %'.4g)\n", Ilat, Ilon, uiuj_F_Phi.at(index) );
                                }
                                coarse_uiuj_F_Psi.at(index) = uiuj_F_Psi_tmp;
                            }

                            if ( mask.at(index) ) {
                                if ( (constants::DO_TIMING) and (tid == 0) ) { clock_on = MPI_Wtime(); }

                                //
                                //// Also get (uiuj)_bar from Cartesian velocities
                                //

                                // tor
                                apply_filter_at_point_for_quadratics(
                                        uxux_tmp, uxuy_tmp, uxuz_tmp, uyuy_tmp, uyuz_tmp, uzuz_tmp, vort_ux_tmp, vort_uy_tmp, vort_uz_tmp,
                                        u_x_tor,  u_y_tor,  u_z_tor, full_vort_tor_r, source_data, Itime, Idepth, Ilat, Ilon,
                                        LAT_lb, LAT_ub, scale, local_kernel);

                                ux_ux_tor.at(index) = uxux_tmp;
                                ux_uy_tor.at(index) = uxuy_tmp;
                                ux_uz_tor.at(index) = uxuz_tmp;
                                uy_uy_tor.at(index) = uyuy_tmp;
                                uy_uz_tor.at(index) = uyuz_tmp;
                                uz_uz_tor.at(index) = uzuz_tmp;

                                vort_ux_tor.at(index) = vort_ux_tmp;
                                vort_uy_tor.at(index) = vort_uy_tmp;
                                vort_uz_tor.at(index) = vort_uz_tmp;

                                KE_tor_filt.at(index) = 0.5 * constants::rho0 * (uxux_tmp + uyuy_tmp + uzuz_tmp);

                                // pot
                                apply_filter_at_point_for_quadratics(
                                        uxux_tmp, uxuy_tmp, uxuz_tmp, uyuy_tmp, uyuz_tmp, uzuz_tmp, vort_ux_tmp, vort_uy_tmp, vort_uz_tmp,
                                        u_x_pot,  u_y_pot,  u_z_pot, full_vort_pot_r, source_data, Itime, Idepth, Ilat, Ilon,
                                        LAT_lb, LAT_ub, scale, local_kernel);

                                ux_ux_pot.at(index) = uxux_tmp;
                                ux_uy_pot.at(index) = uxuy_tmp;
                                ux_uz_pot.at(index) = uxuz_tmp;
                                uy_uy_pot.at(index) = uyuy_tmp;
                                uy_uz_pot.at(index) = uyuz_tmp;
                                uz_uz_pot.at(index) = uzuz_tmp;

                                vort_ux_pot.at(index) = vort_ux_tmp;
                                vort_uy_pot.at(index) = vort_uy_tmp;
                                vort_uz_pot.at(index) = vort_uz_tmp;

                                KE_pot_filt.at(index) = 0.5 * constants::rho0 * (uxux_tmp + uyuy_tmp + uzuz_tmp);

                                // tot
                                apply_filter_at_point_for_quadratics(
                                        uxux_tmp, uxuy_tmp, uxuz_tmp, uyuy_tmp, uyuz_tmp, uzuz_tmp, vort_ux_tmp, vort_uy_tmp, vort_uz_tmp,
                                        u_x_tot,  u_y_tot,  u_z_tot, full_vort_tot_r, source_data, Itime, Idepth, Ilat, Ilon,
                                        LAT_lb, LAT_ub, scale, local_kernel);

                                ux_ux_tot.at(index) = uxux_tmp;
                                ux_uy_tot.at(index) = uxuy_tmp;
                                ux_uz_tot.at(index) = uxuz_tmp;
                                uy_uy_tot.at(index) = uyuy_tmp;
                                uy_uz_tot.at(index) = uyuz_tmp;
                                uz_uz_tot.at(index) = uzuz_tmp;

                                vort_ux_tot.at(index) = vort_ux_tmp;
                                vort_uy_tot.at(index) = vort_uy_tmp;
                                vort_uz_tot.at(index) = vort_uz_tmp;

                                KE_tot_filt.at(index) = 0.5 * constants::rho0 * (uxux_tmp + uyuy_tmp + uzuz_tmp);

                                if ( (constants::DO_TIMING) and (tid == 0) ) { 
                                    timing_records.add_to_record(MPI_Wtime() - clock_on, "filter_at_point_for_quadratics"); 
                                }

                            }  // end if(masked) block
                        }  // end for(depth) block
                    }  // end for(time) block
                }  // end for(longitude) block
            }  // end for(latitude) block
        }  // end pragma parallel block
        #if DEBUG >= 0
        if (wRank == 0) { fprintf(stdout, "\n"); }
        #endif

        #if DEBUG >= 2
        fprintf(stdout, "  = Rank %d finished filtering loop =\n", wRank);
        fflush(stdout);
        #endif

        // Write to file
        if (not(constants::NO_FULL_OUTPUTS)) {
            // Don't mask these fields, since they are filled over land from the projection
            write_field_to_output(coarse_F_tor, "coarse_F_tor", starts, counts, fname, NULL);
            write_field_to_output(coarse_F_pot, "coarse_F_pot", starts, counts, fname, NULL);

            if ( constants::COMP_PI_HELMHOLTZ ) {
                write_field_to_output(coarse_uiuj_F_r,   "coarse_uiuj_F_r",   starts, counts, fname, NULL);
                write_field_to_output(coarse_uiuj_F_Phi, "coarse_uiuj_F_Phi", starts, counts, fname, NULL);
                write_field_to_output(coarse_uiuj_F_Psi, "coarse_uiuj_F_Psi", starts, counts, fname, NULL);
            }
        }

        // Get pot and tor velocities
        if (constants::DO_TIMING) { clock_on = MPI_Wtime(); }
        toroidal_vel_from_F( u_lon_tor, u_lat_tor, coarse_F_tor, longitude, latitude, Ntime, Ndepth, Nlat, Nlon, mask);
        potential_vel_from_F(u_lon_pot, u_lat_pot, coarse_F_pot, longitude, latitude, Ntime, Ndepth, Nlat, Nlon, mask);

        #pragma omp parallel \
        default( none ) \
        shared( mask, u_lon_tor, u_lat_tor, u_lon_pot, u_lat_pot, u_lon_tot, u_lat_tot ) \
        private( index )
        {
            #pragma omp for collapse(1) schedule(dynamic, OMP_chunksize)
            for (index = 0; index < u_lon_tor.size(); ++index) {
                if ( mask.at(index) ) {
                    u_lon_tot.at(index) = u_lon_tor.at(index) + u_lon_pot.at(index);
                    u_lat_tot.at(index) = u_lat_tor.at(index) + u_lat_pot.at(index);
                }
            }
        }
        if (constants::DO_TIMING) { timing_records.add_to_record(MPI_Wtime() - clock_on, "compute velocities from F"); }

        if (not(constants::NO_FULL_OUTPUTS)) {
            write_field_to_output(u_lon_tor, "u_lon_tor", starts, counts, fname, &mask);
            write_field_to_output(u_lat_tor, "u_lat_tor", starts, counts, fname, &mask);

            write_field_to_output(u_lon_pot, "u_lon_pot", starts, counts, fname, &mask);
            write_field_to_output(u_lat_pot, "u_lat_pot", starts, counts, fname, &mask);
        }

        // Get uiuj from corresponding Helmholtz
        if (constants::DO_TIMING) { clock_on = MPI_Wtime(); }
        if ( constants::COMP_PI_HELMHOLTZ ) {
            uiuj_from_Helmholtz( ulon_ulon, ulon_ulat, ulat_ulat, coarse_uiuj_F_r, coarse_uiuj_F_Phi, coarse_uiuj_F_Psi, source_data );

            if (not(constants::MINIMAL_OUTPUT)) {
                write_field_to_output( ulon_ulon, "coarse_uu", starts, counts, fname, &mask );
                write_field_to_output( ulon_ulat, "coarse_uv", starts, counts, fname, &mask );
                write_field_to_output( ulat_ulat, "coarse_vv", starts, counts, fname, &mask );
            }
        }

        // compute_vorticity gives vorticity, divergence, and OkuboWeiss
        if (constants::DO_TIMING) { clock_on = MPI_Wtime(); }
        compute_vorticity(
                vort_tor_r, null_vector, null_vector, div_tor, OkuboWeiss_tor,
                u_r_zero, u_lon_tor, u_lat_tor,
                Ntime, Ndepth, Nlat, Nlon, longitude, latitude, mask);

        compute_vorticity(
                vort_pot_r, null_vector, null_vector, div_pot, OkuboWeiss_pot,
                u_r_zero, u_lon_pot, u_lat_pot,
                Ntime, Ndepth, Nlat, Nlon, longitude, latitude, mask);

        compute_vorticity(
                vort_tot_r, null_vector, null_vector, div_tot, OkuboWeiss_tot,
                u_r_zero, u_lon_tot, u_lat_tot,
                Ntime, Ndepth, Nlat, Nlon, longitude, latitude, mask);
        if (constants::DO_TIMING) { timing_records.add_to_record(MPI_Wtime() - clock_on, "compute vorticity"); }

        if (not(constants::MINIMAL_OUTPUT)) {
            write_field_to_output(div_tor, "div_tor", starts, counts, fname, &mask);
            write_field_to_output(div_pot, "div_pot", starts, counts, fname, &mask);
            write_field_to_output(div_tot, "div_tot", starts, counts, fname, &mask);

            if (constants::DO_OKUBOWEISS_ANALYSIS) {
                write_field_to_output(OkuboWeiss_tor, "OkuboWeiss_tor", starts, counts, fname, &mask);
                write_field_to_output(OkuboWeiss_pot, "OkuboWeiss_pot", starts, counts, fname, &mask);
                write_field_to_output(OkuboWeiss_tot, "OkuboWeiss_tot", starts, counts, fname, &mask);
            }
        }

        //
        //// Toroidal diagnostics
        //

        if (constants::DO_TIMING) { clock_on = MPI_Wtime(); }
        vel_Spher_to_Cart( u_x_coarse, u_y_coarse, u_z_coarse, u_r_zero, u_lon_tor, u_lat_tor, source_data );

        // Energy cascade (Pi)
        compute_Pi( Pi_tor, source_data, u_x_coarse, u_y_coarse, u_z_coarse, ux_ux_tor, ux_uy_tor, ux_uz_tor, uy_uy_tor, uy_uz_tor, uz_uz_tor );
        compute_Pi_shift_deriv( Pi2_tor, source_data, u_x_coarse, u_y_coarse, u_z_coarse, ux_ux_tor, ux_uy_tor, ux_uz_tor, uy_uy_tor, uy_uz_tor, uz_uz_tor );

        // Enstrophy cascade (Z)
        compute_Z(  Z_tor, source_data, u_x_coarse, u_y_coarse, u_z_coarse, vort_tor_r, vort_ux_tor, vort_uy_tor, vort_uz_tor );

        // Energy transport
        compute_div_transport( div_J_tor, u_x_coarse, u_y_coarse, u_z_coarse, ux_ux_tor, ux_uy_tor, ux_uz_tor, uy_uy_tor, uy_uz_tor, uz_uz_tor, u_r_zero,
               longitude, latitude, Ntime, Ndepth, Nlat, Nlon, mask );

        //
        //// Potential diagnostics
        //

        vel_Spher_to_Cart( u_x_coarse, u_y_coarse, u_z_coarse, u_r_zero, u_lon_pot, u_lat_pot, source_data );

        // Energy cascade (Pi)
        compute_Pi( Pi_pot, source_data, u_x_coarse, u_y_coarse, u_z_coarse, ux_ux_pot, ux_uy_pot, ux_uz_pot, uy_uy_pot, uy_uz_pot, uz_uz_pot );
        compute_Pi_shift_deriv( Pi2_pot, source_data, u_x_coarse, u_y_coarse, u_z_coarse, ux_ux_pot, ux_uy_pot, ux_uz_pot, uy_uy_pot, uy_uz_pot, uz_uz_pot );

        // Enstrophy cascade (Z)
        compute_Z(  Z_pot, source_data, u_x_coarse, u_y_coarse, u_z_coarse, vort_pot_r, vort_ux_pot, vort_uy_pot, vort_uz_pot );

        // Energy transport
        compute_div_transport( div_J_pot, u_x_coarse, u_y_coarse, u_z_coarse, ux_ux_pot, ux_uy_pot, ux_uz_pot, uy_uy_pot, uy_uz_pot, uz_uz_pot, u_r_zero,
               longitude, latitude, Ntime, Ndepth, Nlat, Nlon, mask );

        //
        //// Total velocity diagnostics
        //

        vel_Spher_to_Cart( u_x_coarse, u_y_coarse, u_z_coarse, u_r_zero, u_lon_tot, u_lat_tot, source_data );

        // Energy cascade (Pi)
        compute_Pi( Pi_tot, source_data, u_x_coarse, u_y_coarse, u_z_coarse, ux_ux_tot, ux_uy_tot, ux_uz_tot, uy_uy_tot, uy_uz_tot, uz_uz_tot );
        compute_Pi_shift_deriv( Pi2_tot, source_data, u_x_coarse, u_y_coarse, u_z_coarse, ux_ux_tot, ux_uy_tot, ux_uz_tot, uy_uy_tot, uy_uz_tot, uz_uz_tot );

        // Enstrophy cascade (Z)
        compute_Z(  Z_tot, source_data, u_x_coarse, u_y_coarse, u_z_coarse, vort_tot_r, vort_ux_tot, vort_uy_tot, vort_uz_tot );

        // Energy transport
        compute_div_transport( div_J_tot, u_x_coarse, u_y_coarse, u_z_coarse, ux_ux_tot, ux_uy_tot, ux_uz_tot, uy_uy_tot, uy_uz_tot, uz_uz_tot, u_r_zero,
               longitude, latitude, Ntime, Ndepth, Nlat, Nlon, mask );

        //
        if ( constants::COMP_PI_HELMHOLTZ ) {
            compute_Pi_Helmholtz( Pi_Helm, source_data, u_lon_tot, u_lat_tot, ulon_ulon, ulon_ulat, ulat_ulat );
        }
        if (constants::DO_TIMING) { timing_records.add_to_record(MPI_Wtime() - clock_on, "compute_Pi_and_Z"); }

        if (not(constants::NO_FULL_OUTPUTS)) {
            write_field_to_output( Pi_tor, "Pi_tor", starts, counts, fname, &mask);
            write_field_to_output( Pi_pot, "Pi_pot", starts, counts, fname, &mask);
            write_field_to_output( Pi_tot, "Pi_tot", starts, counts, fname, &mask);

            write_field_to_output( Pi2_tor, "Pi2_tor", starts, counts, fname, &mask);
            write_field_to_output( Pi2_pot, "Pi2_pot", starts, counts, fname, &mask);
            write_field_to_output( Pi2_tot, "Pi2_tot", starts, counts, fname, &mask);

            if ( constants::COMP_PI_HELMHOLTZ ) {
                write_field_to_output( Pi_Helm, "Pi_Helm", starts, counts, fname, &mask);
            }

            write_field_to_output( Z_tor, "Z_tor", starts, counts, fname, &mask);
            write_field_to_output( Z_pot, "Z_pot", starts, counts, fname, &mask);
            write_field_to_output( Z_tot, "Z_tot", starts, counts, fname, &mask);
        }

        if (constants::DO_TIMING) { clock_on = MPI_Wtime(); }
        #pragma omp parallel \
        default( none ) \
        shared( KE_tor_coarse, KE_tor_fine, KE_tor_filt, KE_tor_fine_mod, KE_tor_orig, \
                KE_pot_coarse, KE_pot_fine, KE_pot_filt, KE_pot_fine_mod, KE_pot_orig, \
                KE_tot_coarse, KE_tot_fine, KE_tot_filt, KE_tot_fine_mod, KE_tot_orig, \
                Enst_tor, Enst_pot, Enst_tot, mask, \
                u_lon_tor, u_lat_tor, u_lon_pot, u_lat_pot, u_lon_tot, u_lat_tot, \
                vort_tor_r, vort_pot_r, vort_tot_r ) \
        private( index )
        {
            #pragma omp for collapse(1) schedule(dynamic, OMP_chunksize)
            for (index = 0; index < u_lon_tor.size(); ++index) {
                if ( mask.at(index) ) { 
                    KE_tor_coarse.at(index) = 0.5 * constants::rho0 * ( pow(u_lon_tor.at(index), 2.) + pow(u_lat_tor.at(index), 2.) );
                    KE_pot_coarse.at(index) = 0.5 * constants::rho0 * ( pow(u_lon_pot.at(index), 2.) + pow(u_lat_pot.at(index), 2.) );
                    KE_tot_coarse.at(index) = 0.5 * constants::rho0 * ( pow(u_lon_tot.at(index), 2.) + pow(u_lat_tot.at(index), 2.) );

                    KE_tor_fine.at(index) = KE_tor_filt.at(index) - KE_tor_coarse.at(index);
                    KE_pot_fine.at(index) = KE_pot_filt.at(index) - KE_pot_coarse.at(index);
                    KE_tot_fine.at(index) = KE_tot_filt.at(index) - KE_tot_coarse.at(index);

                    KE_tor_fine_mod.at(index) = KE_tor_orig.at(index) - KE_tor_coarse.at(index);
                    KE_pot_fine_mod.at(index) = KE_pot_orig.at(index) - KE_pot_coarse.at(index);
                    KE_tot_fine_mod.at(index) = KE_tot_orig.at(index) - KE_tot_coarse.at(index);

                    Enst_tor.at(index) = 0.5 * constants::rho0 * ( pow(vort_tor_r.at(index), 2.) );
                    Enst_pot.at(index) = 0.5 * constants::rho0 * ( pow(vort_pot_r.at(index), 2.) );
                    Enst_tot.at(index) = 0.5 * constants::rho0 * ( pow(vort_tot_r.at(index), 2.) );
                }
            }
        }
        if (constants::DO_TIMING) { timing_records.add_to_record(MPI_Wtime() - clock_on, "compute KE and Enstrophy"); }

        if (not(constants::NO_FULL_OUTPUTS)) {
            write_field_to_output( KE_tor_filt, "KE_tor_filt", starts, counts, fname, &mask);
            write_field_to_output( KE_pot_filt, "KE_pot_filt", starts, counts, fname, &mask);
            write_field_to_output( KE_tot_filt, "KE_tot_filt", starts, counts, fname, &mask);

            write_field_to_output( KE_tor_fine, "KE_tor_fine", starts, counts, fname, &mask);
            write_field_to_output( KE_pot_fine, "KE_pot_fine", starts, counts, fname, &mask);
            write_field_to_output( KE_tot_fine, "KE_tot_fine", starts, counts, fname, &mask);
        }

        if (not(constants::MINIMAL_OUTPUT)) {
            write_field_to_output( KE_tor_fine_mod, "KE_tor_fine_mod", starts, counts, fname, &mask);
            write_field_to_output( KE_pot_fine_mod, "KE_pot_fine_mod", starts, counts, fname, &mask);
            write_field_to_output( KE_tot_fine_mod, "KE_tot_fine_mod", starts, counts, fname, &mask);

            write_field_to_output( Enst_tor, "Enstrophy_tor", starts, counts, fname, &mask);
            write_field_to_output( Enst_pot, "Enstrophy_pot", starts, counts, fname, &mask);
            write_field_to_output( Enst_tot, "Enstrophy_tot", starts, counts, fname, &mask);

            write_field_to_output( vort_tor_r, "vort_r_tor", starts, counts, fname, &mask);
            write_field_to_output( vort_pot_r, "vort_r_pot", starts, counts, fname, &mask);
            write_field_to_output( vort_tot_r, "vort_r_tot", starts, counts, fname, &mask);
        }

        //
        //// on-line postprocessing, if desired
        //

        if (constants::APPLY_POSTPROCESS) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (constants::DO_TIMING) { clock_on = MPI_Wtime(); }

            #if DEBUG >= 1
            if (wRank == 0) { fprintf(stdout, "Beginning post-process routines\n"); }
            fflush(stdout);
            #endif

            Apply_Postprocess_Routines(
                    source_data, postprocess_fields_tor, postprocess_names, OkuboWeiss_tor,
                    scales.at(Iscale), "postprocess_toroidal");

            Apply_Postprocess_Routines(
                    source_data, postprocess_fields_pot, postprocess_names, OkuboWeiss_pot,
                    scales.at(Iscale), "postprocess_potential");

            Apply_Postprocess_Routines(
                    source_data, postprocess_fields_tot, postprocess_names, OkuboWeiss_tot,
                    scales.at(Iscale), "postprocess_full");

            #if DEBUG >= 1
            if (wRank == 0) { fprintf(stdout, "Finished post-process routines\n"); }
            fflush(stdout);
            #endif

            if (constants::DO_TIMING) { 
                timing_records.add_to_record(MPI_Wtime() - clock_on, "postprocess");
            }
        }

        #if DEBUG >= 0
        // Flushing stdout is necessary for SLURM outputs.
        fflush(stdout);
        #endif

        // If we're doing timings, then print out and reset values now
        if (constants::DO_TIMING) { 
            timing_records.print();
            timing_records.reset();
            fflush(stdout);
        }

    }  // end for(scale) block
} // end filtering
