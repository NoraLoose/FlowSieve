#include "../constants.hpp"
#include "../functions.hpp"
#include "../netcdf_io.hpp"
#include "../preprocess.hpp"
#include "../differentiation_tools.hpp"
#include <algorithm>
#include <vector>
#include <omp.h>
#include <math.h>
#include "../ALGLIB/stdafx.h"
#include "../ALGLIB/linalg.h"
#include "../ALGLIB/solvers.h"

void sparse_vel_from_PsiPhi_vortdiv(
        alglib::sparsematrix & LHS_matr,
        const dataset & source_data,
        const int Itime,
        const int Idepth,
        const std::vector<bool> & mask,
        const bool weight_err,
        const double Tikhov_Laplace,
        const double deriv_scale_factor,
        const int wRank
        ) {

    const std::vector<double>   &latitude   = source_data.latitude,
                                &longitude  = source_data.longitude,
                                &dAreas     = source_data.areas;

    const std::vector<int>  &myCounts = source_data.myCounts;

    const int   Ntime   = myCounts.at(0),
                Ndepth  = myCounts.at(1),
                Nlat    = myCounts.at(2),
                Nlon    = myCounts.at(3);

    int Ilat, Ilon, IDIFF, Idiff, Ndiff, LB;
    size_t index, index_sub, diff_index;
    const size_t Npts = Nlat * Nlon;
    double tmp_val, tan_lat;
    std::vector<double> diff_vec;
    bool is_pole;

    const double R_inv  = 1. / constants::R_earth,
                 R2_inv = pow( R_inv, 2 );


    //
    ////
    ////// Add terms for velocity matching
    ////
    //
    #if DEBUG >= 1
    if (wRank == 0) { fprintf( stdout, "  Adding terms to force velocity matching.\n" ); }
    #endif

    for ( Ilat = 0; Ilat < Nlat; Ilat++ ) {
        for ( Ilon = 0; Ilon < Nlon; Ilon++ ) {
            
            // If we're too close to the pole (less than 0.01 degrees), bad things happen
            is_pole = std::fabs( std::fabs( latitude.at(Ilat) * 180.0 / M_PI ) - 90 ) < 0.01;

            index = Index(Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon);
            index_sub = Index(0, 0, Ilat, Ilon, 1, 1, Nlat, Nlon);
            
            double weight_val = weight_err ? dAreas.at(index_sub) : 1.;

            double cos_lat_inv = 1. / cos(latitude.at(Ilat)),
                   cos2_lat_inv = pow( cos_lat_inv, 2. );

            if ( not(is_pole) ) { // Skip poles

                //
                //// LON first derivative part
                //

                LB = - 2 * Nlon;
                get_diff_vector(diff_vec, LB, longitude, "lon", Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon, mask, 1, constants::DiffOrd);

                Ndiff = diff_vec.size();

                // If LB is unchanged, then we failed to build a stencil
                if (LB != - 2 * Nlon) {
                    for ( IDIFF = LB; IDIFF < LB + Ndiff; IDIFF++ ) {

                        if (constants::PERIODIC_X) { Idiff = ( IDIFF % Nlon + Nlon ) % Nlon; }
                        else                       { Idiff = IDIFF;                          }

                        diff_index = Index(0, 0, Ilat, Idiff, 1, 1, Nlat, Nlon);

                        tmp_val     = diff_vec.at(IDIFF-LB) * cos_lat_inv * R_inv;
                        tmp_val    *= weight_val;

                        // Psi part
                        size_t  column_skip = 0 * Npts,
                                row_skip    = 1 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index, tmp_val );

                        // Phi part
                        column_skip = 1 * Npts;
                        row_skip    = 0 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index, tmp_val );
                    }
                }


                //
                //// LAT first derivative part
                //

                LB = - 2 * Nlat;
                get_diff_vector(diff_vec, LB, latitude, "lat", Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon, mask, 1, constants::DiffOrd);

                Ndiff = diff_vec.size();

                // If LB is unchanged, then we failed to build a stencil
                if (LB != - 2 * Nlat) {
                    for ( IDIFF = LB; IDIFF < LB + Ndiff; IDIFF++ ) {

                        if (constants::PERIODIC_Y) { Idiff = ( IDIFF % Nlat + Nlat ) % Nlat; }
                        else                       { Idiff = IDIFF;                          }

                        diff_index = Index(0, 0, Idiff, Ilon, 1, 1, Nlat,  Nlon);

                        tmp_val     = diff_vec.at(IDIFF-LB) * R_inv;
                        tmp_val    *= weight_val;

                        // Psi part
                        size_t  column_skip = 0 * Npts,
                                row_skip    = 0 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index, -tmp_val );

                        // Phi part
                        column_skip = 1 * Npts;
                        row_skip    = 1 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index,  tmp_val );
                    }
                }
            }
        }
    }



    //
    ////
    ////// Add in Laplace terms to force Phi / Psi to match vorticity and divergence of flow
    ////
    //
    #if DEBUG >= 1
    if (wRank == 0) { fprintf( stdout, "  Adding Laplace terms to force velocity / divergence matching.\n" ); }
    #endif


    for ( Ilat = 0; Ilat < Nlat; Ilat++ ) {
        for ( Ilon = 0; Ilon < Nlon; Ilon++ ) {
            
            // If we're too close to the pole (less than 0.01 degrees), bad things happen
            is_pole = std::fabs( std::fabs( latitude.at(Ilat) * 180.0 / M_PI ) - 90 ) < 0.01;

            index = Index(Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon);
            index_sub = Index(0, 0, Ilat, Ilon, 1, 1, Nlat, Nlon);
            
            double weight_val = weight_err ? dAreas.at(index_sub) : 1.;

            double cos_lat_inv = 1. / cos(latitude.at(Ilat)),
                   cos2_lat_inv = pow( cos_lat_inv, 2. );

            if ( ( Ilat == 0 ) and (Tikhov_Laplace == 0) ) {
                // At the pole-most point, force to be zonally constant. This is to try and remove the null(Laplacian) component
                //      i.e. force neighbouring points to sum to zero

                // i.e. force zero zonal derivative
                LB = - 2 * Nlon;
                get_diff_vector(diff_vec, LB, longitude, "lon", Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon, mask, 1, constants::DiffOrd);

                Ndiff = diff_vec.size();

                // If LB is unchanged, then we failed to build a stencil
                if (LB != - 2 * Nlon) {
                    for ( IDIFF = LB; IDIFF < LB + Ndiff; IDIFF++ ) {

                        if (constants::PERIODIC_X) { Idiff = ( IDIFF % Nlon + Nlon ) % Nlon; }
                        else                       { Idiff = IDIFF;                          }

                        diff_index = Index(0, 0, Ilat, Idiff, 1, 1, Nlat, Nlon);

                        //tmp_val     = diff_vec.at(IDIFF-LB);
                        tmp_val     = diff_vec.at(IDIFF-LB) * cos_lat_inv * R_inv;
                        tmp_val    *= weight_val;

                        // Psi part
                        size_t  column_skip = 1 * Npts,
                                row_skip    = 2 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index, tmp_val );

                        // Phi part
                        column_skip = 0 * Npts;
                        row_skip    = 3 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index, tmp_val );
                    }
                }

            } else if ( (not(is_pole)) and (Tikhov_Laplace > 0) ) {


                //
                //// LON second derivative part
                //

                LB = - 2 * Nlon;
                get_diff_vector(diff_vec, LB, longitude, "lon", Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon, mask, 2, constants::DiffOrd);

                Ndiff = diff_vec.size();

                // If LB is unchanged, then we failed to build a stencil
                if (LB != - 2 * Nlon) {
                    for ( IDIFF = LB; IDIFF < LB + Ndiff; IDIFF++ ) {

                        if (constants::PERIODIC_X) { Idiff = ( IDIFF % Nlon + Nlon ) % Nlon; }
                        else                       { Idiff = IDIFF;                          }

                        diff_index = Index(0, 0, Ilat, Idiff, 1, 1, Nlat, Nlon);

                        tmp_val     = diff_vec.at(IDIFF-LB) * cos2_lat_inv * R2_inv;
                        tmp_val    *= weight_val * Tikhov_Laplace / deriv_scale_factor;

                        // (2,0) entry
                        size_t  row_skip    = 2 * Npts,
                                column_skip = 0 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index, tmp_val );

                        // (3,1) entry
                        row_skip    = 3 * Npts;
                        column_skip = 1 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index, tmp_val );
                    }
                }


                //
                //// LAT second derivative part
                //

                LB = -2 * Nlat;
                get_diff_vector(diff_vec, LB, latitude, "lat", Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon, mask, 2, constants::DiffOrd);

                Ndiff = diff_vec.size();

                // If LB is unchanged, then we failed to build a stencil
                if (LB != - 2 * Nlat) {
                    for ( IDIFF = LB; IDIFF < LB + Ndiff; IDIFF++ ) {

                        if (constants::PERIODIC_Y) { Idiff = ( IDIFF % Nlat + Nlat ) % Nlat; }
                        else                       { Idiff = IDIFF;                          }

                        diff_index = Index(0, 0, Idiff, Ilon, 1, 1, Nlat,  Nlon);

                        tmp_val     = diff_vec.at(IDIFF-LB) * R2_inv;
                        tmp_val    *= weight_val * Tikhov_Laplace / deriv_scale_factor;

                        // (2,0) entry
                        size_t  row_skip    = 2 * Npts,
                                column_skip = 0 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index, tmp_val );

                        // (3,1) entry
                        row_skip    = 3 * Npts;
                        column_skip = 1 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index, tmp_val );
                    }
                }


                //
                //// LAT first derivative part
                //

                LB = - 2 * Nlat;
                get_diff_vector(diff_vec, LB, latitude, "lat", Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon, mask, 1, constants::DiffOrd);

                Ndiff = diff_vec.size();

                // If LB is unchanged, then we failed to build a stencil
                if (LB != - 2 * Nlat) {
                    for ( IDIFF = LB; IDIFF < LB + Ndiff; IDIFF++ ) {

                        if (constants::PERIODIC_Y) { Idiff = ( IDIFF % Nlat + Nlat ) % Nlat; }
                        else                       { Idiff = IDIFF;                          }

                        diff_index = Index(0, 0, Idiff, Ilon, 1, 1, Nlat,  Nlon);

                        tmp_val     = - diff_vec.at(IDIFF-LB) * tan_lat * R2_inv;
                        tmp_val    *= weight_val * Tikhov_Laplace / deriv_scale_factor;

                        // (2,0) entry
                        size_t  row_skip    = 2 * Npts,
                                column_skip = 0 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index, tmp_val );

                        // (3,1) entry
                        row_skip    = 3 * Npts;
                        column_skip = 1 * Npts;
                        alglib::sparseadd(  LHS_matr, row_skip + index_sub, column_skip + diff_index, tmp_val );
                    }
                }
            }
        }
    }
        
}


void Apply_Helmholtz_Projection(
        const std::string output_fname,
        dataset & source_data,
        const std::vector<double> & seed_tor,
        const std::vector<double> & seed_pot,
        const bool single_seed,
        const double rel_tol,
        const int max_iters,
        const bool weight_err,
        const bool use_mask,
        const double Tikhov_Laplace,
        const MPI_Comm comm
        ) {

    int wRank, wSize;
    MPI_Comm_rank( comm, &wRank );
    MPI_Comm_size( comm, &wSize );

    // Create some tidy names for variables
    const std::vector<double>   &time       = source_data.time,
                                &depth      = source_data.depth,
                                &latitude   = source_data.latitude,
                                &longitude  = source_data.longitude,
                                &dAreas     = source_data.areas;

    const std::vector<bool> &mask = source_data.mask;

    const std::vector<int>  &myCounts = source_data.myCounts,
                            &myStarts = source_data.myStarts;

    std::vector<double>   &u_lat = source_data.variables.at("u_lat"),
                          &u_lon = source_data.variables.at("u_lon");

    // Create a 'no mask' mask variable
    //   we'll treat land values as zero velocity
    //   We do this because including land seems
    //   to introduce strong numerical issues
    const std::vector<bool> unmask(mask.size(), true);

    const int   Ntime   = myCounts.at(0),
                Ndepth  = myCounts.at(1),
                Nlat    = myCounts.at(2),
                Nlon    = myCounts.at(3);

    const size_t Npts = Nlat * Nlon;

    int Itime, Idepth, Ilat, Ilon;
    size_t index, index_sub, iters_used;

    // Fill in the land areas with zero velocity
    #pragma omp parallel default(none) shared( u_lon, u_lat, mask, stderr, wRank ) private( index )
    {
        #pragma omp for collapse(1) schedule(guided)
        for (index = 0; index < u_lon.size(); index++) {
            if (not(mask.at(index))) {
                u_lon.at(index) = 0.;
                u_lat.at(index) = 0.;
            } else if (    ( std::fabs( u_lon.at(index) ) > 30000.) 
                        or ( std::fabs( u_lat.at(index) ) > 30000.) 
                      ) {
                fprintf( stderr, "  Rank %d found a bad vel point at index %'zu! Setting to zero.\n", wRank, index );
                u_lon.at(index) = 0.;
                u_lat.at(index) = 0.;
            }
        }
    }

    // Storage vectors
    std::vector<double> 
        full_Psi(        u_lon.size(), 0. ),
        full_Phi(        u_lon.size(), 0. ),
        full_u_lon_tor(  u_lon.size(), 0. ),
        full_u_lat_tor(  u_lon.size(), 0. ),
        full_u_lon_pot(  u_lon.size(), 0. ),
        full_u_lat_pot(  u_lon.size(), 0. ),
        u_lon_tor_seed(  Npts, 0. ),
        u_lat_tor_seed(  Npts, 0. ),
        u_lon_pot_seed(  Npts, 0. ),
        u_lat_pot_seed(  Npts, 0. );

    // alglib variables
    alglib::real_1d_array rhs;
    std::vector<double> 
        RHS_vector( 4 * Npts, 0. ),
        Psi_seed(       Npts, 0. ),
        Phi_seed(       Npts, 0. ),
        work_arr(       Npts, 0. ),
        div_term(       Npts, 0. ),
        vort_term(      Npts, 0. ),
        u_lon_rem(      Npts, 0. ),
        u_lat_rem(      Npts, 0. );
    

    // Copy the starting seed.
    if (single_seed) {
        #pragma omp parallel default(none) shared(Psi_seed, Phi_seed, seed_tor, seed_pot) private( Ilat, Ilon, index )
        {
            #pragma omp for collapse(2) schedule(static)
            for (Ilat = 0; Ilat < Nlat; ++Ilat) {
                for (Ilon = 0; Ilon < Nlon; ++Ilon) {
                    index = Index(0, 0, Ilat, Ilon, 1, 1, Nlat, Nlon);
                    Psi_seed.at(index) = seed_tor.at(index);
                    Phi_seed.at(index) = seed_pot.at(index);
                }
            }
        }
    }

    rhs.attach_to_ptr( 4 * Npts, &RHS_vector[0] );

    alglib::linlsqrstate state;
    alglib::linlsqrreport report;

    alglib::real_1d_array F_alglib;

    double *F_array;

    //
    //// Build the LHS part of the problem
    //      Ordering is: [  u_from_psi      u_from_phi   ] *  [ psi ]   =    [  u   ]
    //                   [  v_from_psi      v_from_phi   ]    [ phi ]        [  v   ]
    //
    //      Ordering is: [           - ddlat   sec(phi) * ddlon   ] *  [ psi ]   =    [     u     ]
    //                   [  sec(phi) * ddlon              ddlat   ]    [ phi ]        [     v     ]
    //                   [           Laplace                  0   ]                   [ vort(u,v) ]
    //                   [                 0            Laplace   ]                   [  div(u,v) ]
    
    #if DEBUG >= 1
    if (wRank == 0) {
        fprintf(stdout, "Building the LHS of the least squares problem.\n");
        fflush(stdout);
    }
    #endif

    alglib::sparsematrix LHS_matr;
    alglib::sparsecreate(4*Npts, 2*Npts, LHS_matr);

    // Get a magnitude for the derivatives, to help normalize the rows of the 
    //  Laplace entries to have similar magnitude to the others.
    int LB = - 2 * Nlat;
    std::vector<double> diff_vec;
    get_diff_vector(diff_vec, LB, latitude, "lat", Itime, Idepth, Nlat/2, 0, Ntime, Ndepth, Nlat, Nlon, unmask, 1, constants::DiffOrd);
    int Ndiff = diff_vec.size();
    double deriv_scale_factor = 0;
    for ( int IDIFF = 0; IDIFF < diff_vec.size(); IDIFF++ ) { deriv_scale_factor += std::fabs( diff_vec.at(IDIFF) ) / Ndiff; }
    if (wRank == 0) { fprintf( stdout, "deriv_scale_factor = %g\n", deriv_scale_factor ); }

    // Put in {u,v}_from_{psi,phi} bits
    //      this assumes that we can use the same operator for all times / depths
    sparse_vel_from_PsiPhi_vortdiv( LHS_matr, source_data, 0, 0, use_mask ? mask : unmask, weight_err, Tikhov_Laplace, deriv_scale_factor, wRank );

    alglib::sparseconverttocrs(LHS_matr);

    #if DEBUG >= 1
    if (wRank == 0) {
        fprintf(stdout, "Declaring the least squares problem.\n");
        fflush(stdout);
    }
    #endif
    alglib::linlsqrcreate(4*Npts, 2*Npts, state);
    alglib::linlsqrsetcond(state, rel_tol, rel_tol, max_iters);

    // Counters to track termination types
    int terminate_count_abs_tol = 0,
        terminate_count_rel_tol = 0,
        terminate_count_max_iter = 0,
        terminate_count_rounding = 0,
        terminate_count_other = 0;

    // Now do the solve!
    for (int Itime = 0; Itime < Ntime; ++Itime) {
        for (int Idepth = 0; Idepth < Ndepth; ++Idepth) {

            if (not(single_seed)) {
                #if DEBUG >= 2
                fprintf( stdout, "Extracting seed.\n" );
                fflush(stdout);
                #endif
                // If single_seed == false, then we were provided seed values, pull out the appropriate values here
                #pragma omp parallel \
                default(none) \
                shared( Psi_seed, Phi_seed, seed_tor, seed_pot, Itime, Idepth ) \
                private( Ilat, Ilon, index, index_sub )
                {
                    #pragma omp for collapse(2) schedule(static)
                    for (Ilat = 0; Ilat < Nlat; ++Ilat) {
                        for (Ilon = 0; Ilon < Nlon; ++Ilon) {
                            index = Index(Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon);
                            index_sub = Index(0, 0, Ilat, Ilon, 1, 1, Nlat, Nlon);
                            Psi_seed.at(index_sub) = seed_tor.at(index);
                            Phi_seed.at(index_sub) = seed_pot.at(index);
                        }
                    }
                }
            }

            // Get velocity from seed
            #if DEBUG >= 3
            fprintf( stdout, "Getting velocities from seed.\n" );
            fflush(stdout);
            #endif
            toroidal_vel_from_F(  u_lon_tor_seed, u_lat_tor_seed, Psi_seed, longitude, latitude, Ntime, Ndepth, Nlat, Nlon, use_mask ? mask : unmask);
            potential_vel_from_F( u_lon_pot_seed, u_lat_pot_seed, Phi_seed, longitude, latitude, Ntime, Ndepth, Nlat, Nlon, use_mask ? mask : unmask);

            #if DEBUG >= 3
            fprintf( stdout, "Subtracting seed velocity to get remaining.\n" );
            fflush(stdout);
            #endif
            #pragma omp parallel default(none) \
            shared( dAreas, Itime, Idepth, RHS_vector, \
                    u_lon, u_lon_tor_seed, u_lon_pot_seed, u_lon_rem, \
                    u_lat, u_lat_tor_seed, u_lat_pot_seed, u_lat_rem ) \
            private( Ilat, Ilon, index, index_sub )
            {
                #pragma omp for collapse(2) schedule(static)
                for (Ilat = 0; Ilat < Nlat; ++Ilat) {
                    for (Ilon = 0; Ilon < Nlon; ++Ilon) {
                        index_sub = Index( 0,     0,      Ilat, Ilon, 1,     1,      Nlat, Nlon);
                        index     = Index( Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon);

                        u_lon_rem.at( index_sub ) = u_lon.at(index) - u_lon_tor_seed.at(index_sub) - u_lon_pot_seed.at(index_sub);
                        u_lat_rem.at( index_sub ) = u_lat.at(index) - u_lat_tor_seed.at(index_sub) - u_lat_pot_seed.at(index_sub);
                    }
                }
            }
            #if DEBUG >= 3
            fprintf( stdout, "Getting divergence and vorticity from remaining velocity.\n" );
            fflush(stdout);
            #endif
            toroidal_vel_div(        div_term, u_lon_rem, u_lat_rem, longitude, latitude,       1, 1, Nlat, Nlon, use_mask ? mask : unmask );
            toroidal_curl_u_dot_er( vort_term, u_lon_rem, u_lat_rem, longitude, latitude, 0, 0, 1, 1, Nlat, Nlon, use_mask ? mask : unmask );

            #if DEBUG >= 2
            if ( wRank == 0 ) {
                fprintf(stdout, "Building the RHS of the least squares problem.\n");
                fflush(stdout);
            }
            #endif

            //
            //// Set up the RHS_vector
            //
            
            double is_pole;
            #pragma omp parallel default(none) \
            shared( dAreas, latitude, Itime, Idepth, RHS_vector, div_term, vort_term, u_lon_rem, u_lat_rem, deriv_scale_factor ) \
            private( Ilat, Ilon, index, index_sub, is_pole )
            {
                #pragma omp for collapse(2) schedule(static)
                for (Ilat = 0; Ilat < Nlat; ++Ilat) {
                    for (Ilon = 0; Ilon < Nlon; ++Ilon) {
                        index_sub = Index( 0,     0,      Ilat, Ilon, 1,     1,      Nlat, Nlon);
                        index     = Index( Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon);

                        is_pole = std::fabs( std::fabs( latitude.at(Ilat) * 180.0 / M_PI ) - 90 ) < 0.01;

                        RHS_vector.at( 0*Npts + index_sub) = u_lon_rem.at(index_sub);
                        RHS_vector.at( 1*Npts + index_sub) = u_lat_rem.at(index_sub);

                        if ( ( Ilat == 0 ) or ( is_pole ) ) {
                            RHS_vector.at( 2*Npts + index_sub) = 0.;
                            RHS_vector.at( 3*Npts + index_sub) = 0.;
                        } else {
                            RHS_vector.at( 2*Npts + index_sub) = vort_term.at(index_sub) * Tikhov_Laplace / deriv_scale_factor;
                            RHS_vector.at( 3*Npts + index_sub) = div_term.at( index_sub) * Tikhov_Laplace / deriv_scale_factor;
                        }

                        if ( weight_err ) {
                            RHS_vector.at( 0*Npts + index_sub) *= dAreas.at(index_sub);
                            RHS_vector.at( 1*Npts + index_sub) *= dAreas.at(index_sub);
                            RHS_vector.at( 2*Npts + index_sub) *= dAreas.at(index_sub);
                            RHS_vector.at( 3*Npts + index_sub) *= dAreas.at(index_sub);
                        }
                    }
                }
            }

            //
            //// Now apply the least-squares solver
            //
            #if DEBUG >= 2
            if ( wRank == 0 ) {
                fprintf(stdout, "Solving the least squares problem.\n");
                fflush(stdout);
            }
            #endif
            alglib::linlsqrsolvesparse(state, LHS_matr, rhs);
            alglib::linlsqrresults(state, F_alglib, report);

            /*    Rep     -   optimization report:
                * Rep.TerminationType completetion code:
                    *  1    ||Rk||<=EpsB*||B||
                    *  4    ||A^T*Rk||/(||A||*||Rk||)<=EpsA
                    *  5    MaxIts steps was taken
                    *  7    rounding errors prevent further progress,
                            X contains best point found so far.
                            (sometimes returned on singular systems)
                    *  8    user requested termination via calling
                            linlsqrrequesttermination()
                * Rep.IterationsCount contains iterations count
                * NMV countains number of matrix-vector calculations
            */

            #if DEBUG >= 1
            if      (report.terminationtype == 1) { fprintf(stdout, "Termination type: absolulte tolerance reached.\n"); }
            else if (report.terminationtype == 4) { fprintf(stdout, "Termination type: relative tolerance reached.\n"); }
            else if (report.terminationtype == 5) { fprintf(stdout, "Termination type: maximum number of iterations reached.\n"); }
            else if (report.terminationtype == 7) { fprintf(stdout, "Termination type: round-off errors prevent further progress.\n"); }
            else if (report.terminationtype == 8) { fprintf(stdout, "Termination type: user requested (?)\n"); }
            else                                  { fprintf(stdout, "Termination type: unknown\n"); }
            #endif
            if      (report.terminationtype == 1) { terminate_count_abs_tol++; }
            else if (report.terminationtype == 4) { terminate_count_rel_tol++; }
            else if (report.terminationtype == 5) { terminate_count_max_iter++; }
            else if (report.terminationtype == 7) { terminate_count_rounding++; }
            else if (report.terminationtype == 8) { terminate_count_other++; }
            else                                  { terminate_count_other++; }

            iters_used = linlsqrpeekiterationscount( state );

            #if DEBUG >= 2
            if ( wRank == 0 ) {
                fprintf(stdout, " Done solving the least squares problem.\n");
                fflush(stdout);
            }
            #endif

            // Extract the solution and add the seed back in
            F_array = F_alglib.getcontent();
            std::vector<double> Psi_vector(F_array,        F_array +     Npts),
                                Phi_vector(F_array + Npts, F_array + 2 * Npts);
            for (size_t ii = 0; ii < Npts; ++ii) {
                Psi_vector.at(ii) += Psi_seed.at(ii);
                Phi_vector.at(ii) += Phi_seed.at(ii);
            }

            // Get velocity associated to computed F field
            #if DEBUG >= 2
            if ( wRank == 0 ) {
                fprintf(stdout, " Extracting velocities and divergence from toroidal field.\n");
                fflush(stdout);
            }
            #endif

            std::vector<double> u_lon_tor(Npts, 0.), u_lat_tor(Npts, 0.), u_lon_pot(Npts, 0.), u_lat_pot(Npts, 0.);
            toroidal_vel_from_F(  u_lon_tor, u_lat_tor, Psi_vector, longitude, latitude, Ntime, Ndepth, Nlat, Nlon, use_mask ? mask : unmask);
            potential_vel_from_F( u_lon_pot, u_lat_pot, Phi_vector, longitude, latitude, Ntime, Ndepth, Nlat, Nlon, use_mask ? mask : unmask);

            //
            //// Store into the full arrays
            //
            #if DEBUG >= 2
            if ( wRank == 0 ) {
                fprintf(stdout, " Storing values into output arrays\n");
                fflush(stdout);
            }
            #endif
            #pragma omp parallel \
            default(none) \
            shared( full_u_lon_tor, u_lon_tor, full_u_lat_tor, u_lat_tor, \
                    full_u_lon_pot, u_lon_pot, full_u_lat_pot, u_lat_pot, \
                    full_Psi, full_Phi, Psi_vector, Phi_vector, \
                    Phi_seed, Psi_seed, \
                    Itime, Idepth ) \
            private( Ilat, Ilon, index, index_sub )
            {
                #pragma omp for collapse(2) schedule(static)
                for (Ilat = 0; Ilat < Nlat; ++Ilat) {
                    for (Ilon = 0; Ilon < Nlon; ++Ilon) {
                        index = Index(Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon);

                        index_sub = Index(0, 0, Ilat, Ilon, 1, 1, Nlat, Nlon);

                        full_u_lon_tor.at(index) = u_lon_tor.at(index_sub) ;
                        full_u_lat_tor.at(index) = u_lat_tor.at(index_sub) ;

                        full_u_lon_pot.at(index) = u_lon_pot.at(index_sub) ;
                        full_u_lat_pot.at(index) = u_lat_pot.at(index_sub) ;

                        full_Psi.at(index) = Psi_vector.at( index_sub );
                        full_Phi.at(index) = Phi_vector.at( index_sub );

                        // If we don't have a seed for the next iteration, use this solution as the seed
                        if (single_seed) {
                            Psi_seed.at(index_sub) = Psi_vector.at(index_sub);
                            Phi_seed.at(index_sub) = Phi_vector.at(index_sub);
                        }
                    }
                }
            }

            #if DEBUG >= 0
            if ( source_data.full_Ndepth > 1 ) {
                fprintf(stdout, "  --  --  Rank %d done depth %d after %'zu iterations\n", wRank, Idepth + myStarts.at(1), iters_used );
                fflush(stdout);
            }
            #endif

        }

        #if DEBUG >= 0
        if ( source_data.full_Ntime > 1 ) {
            fprintf(stdout, " -- Rank %d done time %d after %'zu iterations\n", wRank, Itime + myStarts.at(0), iters_used );
            fflush(stdout);
        }
        #endif
    }

    //
    //// Print termination counts
    //

    int total_count_abs_tol, total_count_rel_tol, total_count_max_iter, total_count_rounding, total_count_other;

    MPI_Reduce( &terminate_count_abs_tol,  &total_count_abs_tol,  1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD );
    MPI_Reduce( &terminate_count_rel_tol,  &total_count_rel_tol,  1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD );
    MPI_Reduce( &terminate_count_max_iter, &total_count_max_iter, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD );
    MPI_Reduce( &terminate_count_rounding, &total_count_rounding, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD );
    MPI_Reduce( &terminate_count_other,    &total_count_other,    1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD );

    #if DEBUG >= 0
    if (wRank == 0) {
        fprintf( stdout, "\n" );
        fprintf( stdout, "Termination counts: %'d from absolute tolerance\n", total_count_abs_tol );
        fprintf( stdout, "                    %'d from relative tolerance\n", total_count_rel_tol );
        fprintf( stdout, "                    %'d from iteration maximum\n", total_count_max_iter );
        fprintf( stdout, "                    %'d from rounding errors \n", total_count_rounding );
        fprintf( stdout, "                    %'d from other causes \n", total_count_other );
        fprintf( stdout, "\n" );
    }
    #endif


    //
    //// Write the output
    //

    const int ndims = 4;
    size_t starts[ndims] = {
        size_t(myStarts.at(0)), size_t(myStarts.at(1)), size_t(myStarts.at(2)), size_t(myStarts.at(3))
    };
    size_t counts[ndims] = { size_t(Ntime), size_t(Ndepth), size_t(Nlat),  size_t(Nlon) };

    std::vector<std::string> vars_to_write;
    if (not(constants::MINIMAL_OUTPUT)) {
        vars_to_write.push_back("u_lon_tor");
        vars_to_write.push_back("u_lat_tor");

        vars_to_write.push_back("u_lon_pot");
        vars_to_write.push_back("u_lat_pot");
    }

    vars_to_write.push_back("Psi");
    vars_to_write.push_back("Phi");

    initialize_output_file( source_data, vars_to_write, output_fname.c_str(), -1);

    if (not(constants::MINIMAL_OUTPUT)) {
        write_field_to_output(full_u_lon_tor,  "u_lon_tor",  starts, counts, output_fname.c_str(), &unmask);
        write_field_to_output(full_u_lat_tor,  "u_lat_tor",  starts, counts, output_fname.c_str(), &unmask);

        write_field_to_output(full_u_lon_pot,  "u_lon_pot",  starts, counts, output_fname.c_str(), &unmask);
        write_field_to_output(full_u_lat_pot,  "u_lat_pot",  starts, counts, output_fname.c_str(), &unmask);
    }

    write_field_to_output(full_Psi, "Psi", starts, counts, output_fname.c_str(), &unmask);
    write_field_to_output(full_Phi, "Phi", starts, counts, output_fname.c_str(), &unmask);

    // Store some solver information
    add_attr_to_file("rel_tol",         rel_tol,                        output_fname.c_str());
    add_attr_to_file("max_iters",       (double) max_iters,             output_fname.c_str());
    add_attr_to_file("diff_order",      (double) constants::DiffOrd,    output_fname.c_str());
    add_attr_to_file("use_mask",        (double) use_mask,              output_fname.c_str());
    add_attr_to_file("weight_err",      (double) weight_err,            output_fname.c_str());
    add_attr_to_file("Tikhov_Laplace",  Tikhov_Laplace,                 output_fname.c_str());


    //
    //// At the very end, compute the L2 and LInf error for each time/depth
    //

    #if DEBUG >= 1
    if (wRank == 0) {
        fprintf(stdout, "Computing the error of the projection.\n");
    }
    #endif

    std::vector<double> projection_2error(      Ntime * Ndepth, 0. ),
                        projection_Inferror(    Ntime * Ndepth, 0. ),
                        velocity_Infnorm(       Ntime * Ndepth, 0. ),
                        projection_KE(          Ntime * Ndepth, 0. ),
                        toroidal_KE(            Ntime * Ndepth, 0. ),
                        potential_KE(           Ntime * Ndepth, 0. ),
                        velocity_2norm(         Ntime * Ndepth, 0. ),
                        tot_areas(              Ntime * Ndepth, 0. );
    double total_area, error2, errorInf, velInf, tor_KE, pot_KE, proj_KE, orig_KE;
    for (int Itime = 0; Itime < Ntime; ++Itime) {
        for (int Idepth = 0; Idepth < Ndepth; ++Idepth) {

            total_area = 0.;
            error2 = 0.;
            tor_KE = 0.;
            pot_KE = 0.;
            proj_KE = 0.;
            orig_KE = 0.;
            errorInf = 0.;
            velInf = 0.;

            #pragma omp parallel \
            default(none) \
            shared( full_u_lon_tor, full_u_lat_tor, full_u_lon_pot, full_u_lat_pot, \
                    u_lon, u_lat, Itime, Idepth, dAreas, latitude ) \
            reduction(+ : total_area, error2, tor_KE, pot_KE, proj_KE, orig_KE) \
            reduction( max : errorInf, velInf )\
            private( Ilat, Ilon, index, index_sub )
            {
                #pragma omp for collapse(2) schedule(static)
                for (Ilat = 0; Ilat < Nlat; ++Ilat) {
                    for (Ilon = 0; Ilon < Nlon; ++Ilon) {
                        index_sub = Index( 0,     0,      Ilat, Ilon, 1,     1,      Nlat, Nlon);
                        index     = Index( Itime, Idepth, Ilat, Ilon, Ntime, Ndepth, Nlat, Nlon);

                        total_area += dAreas.at(index_sub);

                        error2 += dAreas.at(index_sub) * (
                                        pow( u_lon.at(index) - full_u_lon_tor.at(index) - full_u_lon_pot.at(index) , 2.)
                                     +  pow( u_lat.at(index) - full_u_lat_tor.at(index) - full_u_lat_pot.at(index) , 2.)
                                );

                        errorInf = std::fmax( 
                                        errorInf,
                                        sqrt(     pow( u_lon.at(index) - full_u_lon_tor.at(index) - full_u_lon_pot.at(index) , 2.)
                                               +  pow( u_lat.at(index) - full_u_lat_tor.at(index) - full_u_lat_pot.at(index) , 2.)
                                             )
                                        );

                        velInf = std::fmax( velInf,  std::fabs( sqrt( pow( u_lon.at(index) , 2.) +  pow( u_lat.at(index) , 2.) ) )  );

                        tor_KE += dAreas.at(index_sub) * ( pow( full_u_lon_tor.at(index), 2.) + pow( full_u_lat_tor.at(index), 2.) );
                        pot_KE += dAreas.at(index_sub) * ( pow( full_u_lon_pot.at(index), 2.) + pow( full_u_lat_pot.at(index), 2.) );

                        proj_KE += dAreas.at(index_sub) * (
                                        pow( full_u_lon_tor.at(index) + full_u_lon_pot.at(index) , 2.)
                                     +  pow( full_u_lat_tor.at(index) + full_u_lat_pot.at(index) , 2.)
                                );

                        orig_KE += dAreas.at(index_sub) * ( pow( u_lon.at(index), 2.) + pow( u_lat.at(index), 2.) );
                    }
                }
            }
            size_t int_index = Index( Itime, Idepth, 0, 0, Ntime, Ndepth, 1, 1);

            tot_areas.at(int_index) = total_area;

            projection_2error.at(   int_index ) = sqrt( error2   / total_area );
            projection_Inferror.at( int_index ) = errorInf;

            velocity_2norm.at(   int_index ) = sqrt( orig_KE  / total_area );
            velocity_Infnorm.at( int_index ) = velInf;

            projection_KE.at( int_index ) = sqrt( proj_KE  / total_area );
            toroidal_KE.at(   int_index ) = sqrt( tor_KE   / total_area );
            potential_KE.at(  int_index ) = sqrt( pot_KE   / total_area );
        }
    }

    const char* dim_names[] = {"time", "depth"};
    const int ndims_error = 2;
    if (wRank == 0) {
        add_var_to_file( "total_area",    dim_names, ndims_error, output_fname.c_str() );

        add_var_to_file( "projection_2error",    dim_names, ndims_error, output_fname.c_str() );
        add_var_to_file( "projection_Inferror",  dim_names, ndims_error, output_fname.c_str() );

        add_var_to_file( "velocity_2norm",   dim_names, ndims_error, output_fname.c_str() );
        add_var_to_file( "velocity_Infnorm", dim_names, ndims_error, output_fname.c_str() );

        add_var_to_file( "projection_KE",  dim_names, ndims_error, output_fname.c_str() );
        add_var_to_file( "toroidal_KE",    dim_names, ndims_error, output_fname.c_str() );
        add_var_to_file( "potential_KE",   dim_names, ndims_error, output_fname.c_str() );
    }
    MPI_Barrier(MPI_COMM_WORLD);

    size_t starts_error[ndims_error] = { size_t(myStarts.at(0)), size_t(myStarts.at(1)) };
    size_t counts_error[ndims_error] = { size_t(Ntime), size_t(Ndepth) };

    write_field_to_output( tot_areas,   "total_area",   starts_error, counts_error, output_fname.c_str() );

    write_field_to_output( projection_2error,   "projection_2error",   starts_error, counts_error, output_fname.c_str() );
    write_field_to_output( projection_Inferror, "projection_Inferror", starts_error, counts_error, output_fname.c_str() );

    write_field_to_output( velocity_2norm,   "velocity_2norm",   starts_error, counts_error, output_fname.c_str() );
    write_field_to_output( velocity_Infnorm, "velocity_Infnorm", starts_error, counts_error, output_fname.c_str() );

    write_field_to_output( projection_KE, "projection_KE", starts_error, counts_error, output_fname.c_str() );
    write_field_to_output( toroidal_KE,   "toroidal_KE",   starts_error, counts_error, output_fname.c_str() );
    write_field_to_output( potential_KE,  "potential_KE",  starts_error, counts_error, output_fname.c_str() );

}
