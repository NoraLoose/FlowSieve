import numpy as np
import matplotlib as mpl
import matplotlib.pyplot as plt
from netCDF4 import Dataset
import PlotTools, cmocean
import matpy as mp
import sys, os, shutil, datetime

try: # Try using mpi
    from mpi4py import MPI
    comm = MPI.COMM_WORLD
    rank = comm.Get_rank()
    num_procs = comm.Get_size()
except:
    rank = 0
    num_procs = 1
print("Proc {0:d} of {1:d}".format(rank+1,num_procs))

# If the Figures directory doesn't exist, create it.
# Same with the Figures/tmp
out_direct = os.getcwd() + '/Videos'
tmp_direct = out_direct + '/tmp'

if (rank == 0):
    print("Saving outputs to " + out_direct)
    print("  will use temporary directory " + tmp_direct)

    if not(os.path.exists(out_direct)):
        os.makedirs(out_direct)

    if not(os.path.exists(tmp_direct)):
        os.makedirs(tmp_direct)

fp = 'filter_output.nc'
results = Dataset(fp, 'r')

rho0    = 1e3
R_earth = 6371e3
D2R     = np.pi / 180
R2D     = 180 / np.pi
eps     = 1e-10

# Get the grid from the first filter
latitude  = results.variables['latitude'][:] * R2D
longitude = results.variables['longitude'][:] * R2D
scales    = results.variables['scale'][:]
depth     = results.variables['depth'][:]
time      = results.variables['time'][:] * (60 * 60) # time was in hours
mask      = results.variables['mask'][:]
v_r       = results.variables['vort_r'  ][:]
v_lon     = results.variables['vort_lon'][:]
v_lat     = results.variables['vort_lat'][:]
Lambda    = results.variables['baroclinic_transfer'][:]

Lambda = Lambda[:-1,:,:,:,:]

# Do some time handling tp adjust the epochs
# appropriately
epoch = datetime.datetime(1950,1,1)   # the epoch of the time dimension
dt_epoch = datetime.datetime.fromtimestamp(0)  # the epoch used by datetime
epoch_delta = dt_epoch - epoch  # difference
time = time - epoch_delta.total_seconds()  # shift

Nscales, Ntime, Ndepth, Nlat, Nlon = v_r.shape

dlat = (latitude[1]  - latitude[0] ) * D2R
dlon = (longitude[1] - longitude[0]) * D2R
LON, LAT = np.meshgrid(longitude * D2R, latitude * D2R)
dAreas = R_earth**2 * np.cos(LAT) * dlat * dlon

dArea = np.tile((mask*dAreas).reshape(1,Nlat,Nlon), (Ndepth,1,1)) 
mask  = np.tile(mask.reshape(1,Nlat,Nlon), (Ndepth, 1, 1))

if Ntime >= 5:
    order = 4
else:
    order = Ntime - 1
Dt = mp.FiniteDiff(time, order, spb=False)

net_EN_flux = np.zeros((Ntime, Nscales-1))
net_lambda  = np.zeros((Ntime, Nscales-1))

## Scatter of fluxes vs Lambda
for iS in range(Nscales - 1):
    # First, sort out data
    EN_from_vort = 0.5 * rho0 * (  np.sum(v_r[  iS+1:,:,:,:,:], axis=0)**2 
                                 + np.sum(v_lat[iS+1:,:,:,:,:], axis=0)**2 
                                 + np.sum(v_lon[iS+1:,:,:,:,:], axis=0)**2 )
    EN_from_vort = EN_from_vort.reshape(Ntime, Ndepth*Nlat*Nlon)
    EN_flux = np.matmul(Dt, EN_from_vort)

    for iT in range(Ntime):

        timestamp = datetime.datetime.fromtimestamp(time[iT])
        sup_title = "{0:02d} - {1:02d} - {2:04d} ( {3:02d}:{4:02d} )".format(
                timestamp.day, timestamp.month, timestamp.year, 
                timestamp.hour, timestamp.minute)

        net_EN_flux[iT,iS] = np.sum(EN_flux[iT,:] * dArea.ravel() * mask.ravel())
        net_lambda[ iT,iS] = np.sum(Lambda[iS,iT,:] * dArea         * mask)

        EN_sel     = EN_flux[iT,:].ravel()[mask.ravel() == 1]
        Lambda_sel = Lambda[iS,iT,:,:,:].ravel()[mask.ravel() == 1]
    
        # Then plot
        # Initialize figure
        fig, axes = plt.subplots(2, 2, 
                gridspec_kw = dict(left = 0.1, right = 0.95, bottom = 0.1, top = 0.95,
                    hspace=0.02, wspace=0.02),
                figsize=(7.5, 6) )
        
        PlotTools.SignedLogScatter_hist(Lambda_sel, EN_sel, axes,
                force_equal = True, nbins_x = 200, nbins_y = 200)
        
        for II in range(2):
            axes[II,0].set_ylabel('$\\frac{d}{dt}\left(\\frac{1}{2}\overline{\omega}\cdot\overline{\omega}\\right)$ $(\mathrm{W}^{\omega}\cdot\mathrm{m}^{-3})$')
            axes[1,II].set_xlabel('$\Lambda^m$ $(\mathrm{W}^{\omega}\cdot\mathrm{m}^{-3})$')
        
        for ax in axes.ravel():
            xlim = ax.get_xlim()
            ylim = ax.get_ylim()
            ax.plot(xlim, xlim,'--c', label='$1:1$')
            ax.set_xlim(xlim)
            ax.set_ylim(ylim)
        
        axes[0,1].legend(loc='best')
        
        axes[0,0].set_xticklabels([])
        axes[0,1].set_xticklabels([])
        axes[0,1].set_yticklabels([])
        axes[1,1].set_yticklabels([])

        fig.suptitle(sup_title)
        
        plt.savefig(tmp_direct + '/EN_fluxes_{0:.3g}km_{1:04d}.png'.format(scales[iS]/1e3,iT), dpi=500)
        plt.close()
    
# Now plot the space-integrated version
colours = plt.rcParams['axes.prop_cycle'].by_key()['color']
fig, axes = plt.subplots(2, 1,
        gridspec_kw = dict(left = 0.15, right = 0.95, bottom = 0.1, top = 0.95,
        hspace=0.1))
for iS in range(Nscales-1):
    l1, = axes[0].plot(time, net_EN_flux, '-', color=colours[iS])
    l2, = axes[0].plot(time, net_lambda, '--', color=colours[iS])

    axes[1].plot([iS+0.25,iS+0.75], [0.5, 0.5], '-',  color = colours[iS])
    axes[1].plot([iS+0.25,iS+0.75], [1.5, 1.5], '--', color = colours[iS])

axes[1].set_xlim(0,Nscales-1)
axes[1].set_ylim(0,2)
axes[1].set_xticks(np.arange(Nscales-1)+0.5)
axes[1].set_yticks([0.5, 1.5])
axes[1].set_xticklabels(["{0:.3g}km".format(sc/1e3) for sc in scales[:-1]])
axes[1].set_yticklabels([
    '$\int_{\Omega}\\frac{d}{dt}\left(\\frac{1}{2}\overline{\omega}\cdot\overline{\omega}\\right)$',
   '$\int_{\Omega}\Lambda^m$'], rotation=0, verticalalignment='center')

axes[0].set_ylabel('$\mathrm{W}^{\omega}$')
plt.savefig('Figures/EN_fluxes_net.pdf')
plt.close()

# If more than one time point, create mp4s
if Ntime > 1:
    for iS in range(Nscales-1):
        PlotTools.merge_to_mp4(
                tmp_direct + '/EN_fluxes_{0:.3g}km_%04d.png'.format(scales[iS]/1e3),
                out_direct + '/EN_fluxes_{0:.3g}km.mp4'.format(scales[iS]/1e3),
                fps=12)
else:
    for iS in range(Nscales-1):
        shutilmove(tmp_direct + '/EN_fluxes_{0:.3g}km_%04d.png'.format(scales[iS]/1e3),
                out_direct + '/EN_fluxes_{0:.3g}km.mp4'.format(scales[iS]/1e3))

# Now delete the frames
shutil.rmtree(tmp_direct)
