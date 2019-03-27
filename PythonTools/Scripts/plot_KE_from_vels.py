import numpy as np
import matplotlib as mpl
import matplotlib.pyplot as plt
import cmocean, sys, os, shutil, datetime
from netCDF4 import Dataset
import PlotTools
from matplotlib.colors import LogNorm
from matplotlib.colors import ListedColormap

dpi = PlotTools.dpi

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

source = Dataset('input.nc', 'r')

fp = 'filter_output.nc'
results = Dataset(fp, 'r')

R_earth = 6371e3
D2R     = np.pi / 180
R2D     = 180 / np.pi
eps     = 1e-10

# Create cmap for mask data
ref_cmap = cmocean.cm.gray
mask_cmap = ref_cmap(np.arange(ref_cmap.N))
mask_cmap[:,-1] = np.linspace(1, 0, ref_cmap.N)
mask_cmap = ListedColormap(mask_cmap)

# Get the grid from the first filter
latitude  = results.variables['latitude'][:] * R2D
longitude = results.variables['longitude'][:] * R2D
scales    = results.variables['scale'][:]
depth     = results.variables['depth'][:]
time      = results.variables['time'][:] * 60 * 60 # hours to seconds
mask      = results.variables['mask'][:]

# Do some time handling tp adjust the epochs
# appropriately
epoch = datetime.datetime(1950,1,1)   # the epoch of the time dimension
dt_epoch = datetime.datetime.fromtimestamp(0)  # the epoch used by datetime
epoch_delta = dt_epoch - epoch  # difference
time = time - epoch_delta.total_seconds()  # shift

Ntime = len(time)
num_scales = len(scales)

dlat = (latitude[1]  - latitude[0] ) * D2R
dlon = (longitude[1] - longitude[0]) * D2R
LON, LAT = np.meshgrid(longitude * D2R, latitude * D2R)

# Some parameters for plotting
proj = PlotTools.MapProjection(longitude, latitude)
Xp, Yp = proj(LON * R2D, LAT * R2D, inverse=False)

meridians = np.round(np.linspace(longitude.min(), longitude.max(), 5))
parallels = np.round(np.linspace(latitude.min(),  latitude.max(),  5))

cbar_props = dict(pad = 0.1, orientation = 'vertical')
gridspec_props = dict(wspace = 0.05, hspace = 0.07, left = 0.1, right = 0.9, bottom = 0.1, top = 0.9)


##
## Begin Plotting
##

## KE binning
for Itime in range(rank, Ntime, num_procs):    

    timestamp = datetime.datetime.fromtimestamp(time[Itime])
    sup_title = "{0:02d} - {1:02d} - {2:04d} ( {3:02d}:{4:02d} )".format(
            timestamp.day, timestamp.month, timestamp.year, 
            timestamp.hour, timestamp.minute)
    
    u_r   = results.variables['u_r'  ][:, Itime, 0, :, :]
    u_lon = results.variables['u_lon'][:, Itime, 0, :, :]
    u_lat = results.variables['u_lat'][:, Itime, 0, :, :]
    
    KE = 0.5 * ( u_r**2 + u_lon**2 + u_lat**2 )
    
    uo = source.variables['uo'][Itime, 0, :, :]
    vo = source.variables['vo'][Itime, 0, :, :]
    Full_KE = 0.5 * (uo**2 + vo**2)

    # Initialize figure
    fig, axes = plt.subplots(num_scales+1, 1,
            sharex=True, sharey=True, squeeze=False,
            gridspec_kw = gridspec_props,
            figsize=(6,4*(num_scales+1)))

    fig.suptitle(sup_title)
    
    # Plot each band
    for ii in range(num_scales+1):
        
        if (ii == num_scales):
            to_plot = Full_KE - 0.5 * (   np.sum(u_r,   axis=0)**2 
                                        + np.sum(u_lat, axis=0)**2 
                                        + np.sum(u_lon, axis=0)**2 )
        else:
            to_plot = KE[ii,:,:]
        to_plot = np.ma.masked_where(mask==0, to_plot)
    
        CV  = np.nanmax(np.abs(to_plot))
        if (CV == 0):
            CV = 1
        KE_min = np.min(to_plot)
        if (KE_min < -1e-15) :
            print("min(KE) = {0:.4g} < 1e-16!".format(KE_min), ii)
            qm = axes[ii,0].pcolormesh(Xp, Yp, to_plot, cmap='cmo.balance', vmin = -CV, vmax = CV)
        else:
            qm = axes[ii,0].pcolormesh(Xp, Yp, to_plot, cmap='cmo.amp', vmin = 0, vmax = CV)
        
        cbar = plt.colorbar(qm, ax = axes[ii,0], **cbar_props)
        PlotTools.ScientificCbar(cbar, units='')
    
        # Add coastlines and lat/lon lines
        axes[ii,0].pcolormesh(Xp, Yp, mask, vmin=-1, vmax=1, cmap=mask_cmap)
        PlotTools.AddParallels_and_Meridians(axes[ii,0], proj, 
            parallels, meridians, latitude, longitude, label_meridians=(ii==num_scales))
    
        if (ii == 0):
            axes[ii,0].set_ylabel('Below {0:0.1f} km'.format(scales[0] / 1e3))
        elif (ii == num_scales):
            axes[ii,0].set_ylabel('Orig - bandsum')
        elif (ii == num_scales - 1):
            axes[ii,0].set_ylabel('Above {0:0.1f} km'.format(scales[ii-1] / 1e3))
        else:
            axes[ii,0].set_ylabel('{0:.1f} to {1:0.1f} km'.format(scales[ii-1] / 1e3, scales[ii] / 1e3))

    plt.savefig(tmp_direct + '/KE_bands_from_vels_{0:04d}.png'.format(Itime), dpi=dpi)
    plt.close()
    
## KE dichotomies
for Itime in range(rank, Ntime, num_procs):    

    timestamp = datetime.datetime.fromtimestamp(time[Itime])
    sup_title = "{0:02d} - {1:02d} - {2:04d} ( {3:02d}:{4:02d} )".format(
            timestamp.day, timestamp.month, timestamp.year, 
            timestamp.hour, timestamp.minute)
    
    u_r   = results.variables['u_r'  ][:, Itime, 0, :, :]
    u_lon = results.variables['u_lon'][:, Itime, 0, :, :]
    u_lat = results.variables['u_lat'][:, Itime, 0, :, :]
    
    KE = 0.5 * ( u_r**2 + u_lon**2 + u_lat**2 )
    
    uo = source.variables['uo'][Itime, 0, :, :]
    vo = source.variables['vo'][Itime, 0, :, :]
    Full_KE = 0.5 * (uo**2 + vo**2)
    
    # Plot each band
    for ii in range(num_scales-1):
    
        # Initialize figure
        fig, axes = plt.subplots(3, 1,
            sharex=True, sharey=True, squeeze=False,
            gridspec_kw = gridspec_props,
            figsize=(10, 15))

        fig.suptitle(sup_title)
        
        to_plot_below = 0.5 * (   np.sum(u_r[  :ii+1, :,:], axis=0)**2 
                                + np.sum(u_lat[:ii+1, :,:], axis=0)**2 
                                + np.sum(u_lon[:ii+1, :,:], axis=0)**2 )
        to_plot_above = 0.5 * (   np.sum(u_r[   ii+1:,:,:], axis=0)**2 
                                + np.sum(u_lat[ ii+1:,:,:], axis=0)**2 
                                + np.sum(u_lon[ ii+1:,:,:], axis=0)**2 )
        missing = Full_KE - (to_plot_below + to_plot_above)
    
        to_plot_below = np.ma.masked_where(mask==0, to_plot_below)
        to_plot_above = np.ma.masked_where(mask==0, to_plot_above)
        missing       = np.ma.masked_where(mask==0, missing)
    
        CV_a = np.nanmax(np.abs(to_plot_above))
        CV_b = np.nanmax(np.abs(to_plot_below))
        CV_m = np.nanmax(np.abs(missing))
    
        vmax = max(CV_a, CV_b, CV_m)
        vmin = 10**(np.log10(vmax) - 3)
    
        qm_a = axes[0,0].pcolormesh(Xp, Yp, to_plot_above, cmap='cmo.amp',
                norm=LogNorm(vmin=vmin, vmax=vmax))
    
        qm_b = axes[1,0].pcolormesh(Xp, Yp, to_plot_below, cmap='cmo.amp',
                norm=LogNorm(vmin=vmin, vmax=vmax))
    
        qm_m = axes[2,0].pcolormesh(Xp, Yp, missing, cmap='cmo.amp',
                norm=LogNorm(vmin=vmin, vmax=vmax))
    
        cbar = plt.colorbar(qm_b, ax = axes[:,0], **cbar_props)
        #PlotTools.ScientificCbar(cbar_b, units='')
    
        # Add coastlines and lat/lon lines
        for ax in axes[:,0]:
            ax.pcolormesh(Xp, Yp, mask, vmin=-1, vmax=1, cmap=mask_cmap)
            PlotTools.AddParallels_and_Meridians(ax, proj, 
                parallels, meridians, latitude, longitude, label_meridians=(ax==axes[2,0]))
    
        axes[0,0].set_ylabel('Coarse $(>l)$')
        axes[1,0].set_ylabel('Fine $(<l)$')
        axes[2,0].set_ylabel('Missing')

        plt.savefig(tmp_direct + '/{0:.4g}_KE_dichotomies_from_vels_{1:04d}.png'.format(scales[ii]/1e3,Itime), dpi=dpi)
        plt.close()
    

# If more than one time point, create mp4s
if Ntime > 1:
    PlotTools.merge_to_mp4(tmp_direct + '/KE_bands_from_vels_%04d.png',    
            out_direct + '/KE_bands_from_vels.mp4', fps=12)
    for ii in range(num_scales-1):
        PlotTools.merge_to_mp4(tmp_direct + '/{0:.4g}_KE_dichotomies_from_vels_%04d.png'.format(scales[ii]/1e3),    
                out_direct + '/{0:.4g}km/KE_dichotomies_from_vels.mp4'.format(scales[ii]/1e3), fps=12)

    
else:
    shutil.move(tmp_direct + '/KE_bands_from_vels_0000.png',
            out_direct + '/KE_bands_from_vels.png')
    shutil.move(tmp_direct + '/KE_dichotomies_from_vels_0000.png',
            out_direct + '/KE_dichotomies_from_vels.png')

# Now delete the frames
shutil.rmtree(tmp_direct)

## If Ntime > 1, then also plot time means

## KE dichotomies
if Ntime > 1:

    u_r   = results.variables['u_r'  ][:, :, 0, :, :]
    u_lon = results.variables['u_lon'][:, :, 0, :, :]
    u_lat = results.variables['u_lat'][:, :, 0, :, :]
    
    uo = source.variables['uo'][:, 0, :, :]
    vo = source.variables['vo'][:, 0, :, :]
    Full_KE = np.mean(0.5 * (uo**2 + vo**2), axis=0)
    
    # Plot each band
    for ii in range(num_scales-1):
    
        # Initialize figure
        fig, axes = plt.subplots(3, 1,
            sharex=True, sharey=True, squeeze=False,
            gridspec_kw = gridspec_props,
            figsize=(10, 15))

        fig.suptitle('Time Averaged')
        
        to_plot_below = 0.5 * (   np.sum(u_r[  :ii+1, :,:], axis=0)**2 
                                + np.sum(u_lat[:ii+1, :,:], axis=0)**2 
                                + np.sum(u_lon[:ii+1, :,:], axis=0)**2 )
        to_plot_above = 0.5 * (   np.sum(u_r[   ii+1:,:,:], axis=0)**2 
                                + np.sum(u_lat[ ii+1:,:,:], axis=0)**2 
                                + np.sum(u_lon[ ii+1:,:,:], axis=0)**2 )
        to_plot_below = np.mean(to_plot_below, axis=0)
        to_plot_above = np.mean(to_plot_above, axis=0)

        missing = Full_KE - (to_plot_below + to_plot_above)
    
        to_plot_below = np.ma.masked_where(mask==0, to_plot_below)
        to_plot_above = np.ma.masked_where(mask==0, to_plot_above)
        missing       = np.ma.masked_where(mask==0, missing)
    
        CV_a = np.nanmax(np.abs(to_plot_above))
        CV_b = np.nanmax(np.abs(to_plot_below))
        CV_m = np.nanmax(np.abs(missing))
    
        vmax = max(CV_a, CV_b, CV_m)
        vmin = 10**(np.log10(vmax) - 3)
    
        qm_a = axes[0,0].pcolormesh(Xp, Yp, to_plot_above, cmap='cmo.amp',
                norm=LogNorm(vmin=vmin, vmax=vmax))
    
        qm_b = axes[1,0].pcolormesh(Xp, Yp, to_plot_below, cmap='cmo.amp',
                norm=LogNorm(vmin=vmin, vmax=vmax))
    
        qm_m = axes[2,0].pcolormesh(Xp, Yp, missing, cmap='cmo.amp',
                norm=LogNorm(vmin=vmin, vmax=vmax))
    
        cbar = plt.colorbar(qm_b, ax = axes[:,0], **cbar_props)
    
        # Add coastlines and lat/lon lines
        for ax in axes[:,0]:
            ax.pcolormesh(Xp, Yp, mask, vmin=-1, vmax=1, cmap=mask_cmap)
            PlotTools.AddParallels_and_Meridians(ax, proj, 
                parallels, meridians, latitude, longitude, label_meridians=(ax==axes[2,0]))

        axes[0,0].set_ylabel('Coarse $(>l)$')
        axes[1,0].set_ylabel('Fine $(<l)$')
        axes[2,0].set_ylabel('Missing')
    
        plt.savefig(out_direct + '/{0:.4g}km/AVE_KE_dichotomies_from_vels.png'.format(scales[ii]/1e3), dpi=dpi)
        plt.close()
    

