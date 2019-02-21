/*
 *
 * Index is a function to convert a four-point (physical) index
 *   (Itime, Idepth, Ilat, Ilon) into a one-point (logical) index
 *   to access the double arrays.
 *
 */

#include "../functions.hpp"

int Index( const int Itime, const int Idepth, const int Ilat, const int Ilon,
           const int Ntime, const int Ndepth, const int Nlat, const int Nlon  ){

    int index =   Itime  * (Ndepth * Nlat * Nlon)
                + Idepth * (         Nlat * Nlon)
                + Ilat   * (                Nlon)
                + Ilon;

    return index;

}
