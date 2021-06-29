/*********************************************************************************************
 ZPIC
 emf.c

 Created by Ricardo Fonseca on 10/8/10.
 Modified by Nicolas Guidotti on 11/06/2020

 Copyright 2020 Centro de Física dos Plasmas. All rights reserved.

 *********************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>

#include "emf.h"
#include "zdf.h"
#include "timer.h"
#include "utilities.h"

/*********************************************************************************************
 Constructor / Destructor
 *********************************************************************************************/
void emf_new(t_emf *emf, int nx[], t_fld box[], const float dt, const int device)
{
	int i;

	// Number of guard cells for linear interpolation
	int gc[2][2] = {{1, 2}, {1, 2}};

	// Allocate global arrays
	size_t size;

	size = (gc[0][0] + nx[0] + gc[0][1]) * (gc[1][0] + nx[1] + gc[1][1]);
	emf->total_size = size;
	emf->overlap_size = (gc[0][0] + nx[0] + gc[0][1]) * (gc[1][0] + gc[1][1]);

	emf->E_buf = alloc_device_buffer(size * sizeof(t_vfld), device);
	emf->B_buf = alloc_device_buffer(size * sizeof(t_vfld), device);

	assert(emf->E_buf && emf->B_buf);

	memset(emf->E_buf, 0, size * sizeof(t_vfld));
	memset(emf->B_buf, 0, size * sizeof(t_vfld));

	// store nx and gc values
	for (i = 0; i < 2; i++)
	{
		emf->nx[i] = nx[i];
		emf->gc[i][0] = gc[i][0];
		emf->gc[i][1] = gc[i][1];
	}
	emf->nrow = gc[0][0] + nx[0] + gc[0][1];

	// store time step values
	emf->dt = dt;

	// Make E and B point to cell [0][0]
	emf->E = emf->E_buf + gc[0][0] + gc[1][0] * emf->nrow;
	emf->B = emf->B_buf + gc[0][0] + gc[1][0] * emf->nrow;

	// Set cell sizes and box limits
	for (i = 0; i < 2; i++)
	{
		emf->box[i] = box[i];
		emf->dx[i] = box[i] / nx[i];
	}

	// Set time step
	emf->dt = dt;

	// Reset iteration number
	emf->iter = 0;

	// Reset moving window information
	emf->moving_window = false;
	emf->n_move = 0;
}

// Set the overlap zone between regions (below zone only)
void emf_overlap_zone(t_emf *emf, t_emf *below, const int device)
{
	emf->B_below = below->B + (below->nx[1] - below->gc[1][0]) * below->nrow;
	emf->E_below = below->E + (below->nx[1] - below->gc[1][0]) * below->nrow;

#ifdef ENABLE_ADVISE
	cuMemAdvise(emf->B_below - emf->gc[0][0], emf->overlap_size, CU_MEM_ADVISE_SET_ACCESSED_BY, device);
	cuMemAdvise(emf->E_below - emf->gc[0][0], emf->overlap_size, CU_MEM_ADVISE_SET_ACCESSED_BY, device);
#endif
}

void emf_delete(t_emf *emf)
{
	free_device_buffer(emf->E_buf);
	free_device_buffer(emf->B_buf);

	emf->E_buf = NULL;
	emf->B_buf = NULL;
}

/*********************************************************************************************
 Laser Pulses
 *********************************************************************************************/

t_fld gauss_phase(const t_emf_laser *const laser, const t_fld z, const t_fld r)
{
	t_fld z0 = laser->omega0 * (laser->W0 * laser->W0) / 2;
	t_fld rho2 = r * r;
	t_fld curv = rho2 * z / (z0 * z0 + z * z);
	t_fld rWl2 = (z0 * z0) / (z0 * z0 + z * z);
	t_fld gouy_shift = atan2(z, z0);

	return sqrt(sqrt(rWl2)) * exp(-rho2 * rWl2 / (laser->W0 * laser->W0))
			* cos(laser->omega0 * (z + curv) - gouy_shift);
}

t_fld lon_env(const t_emf_laser *const laser, const t_fld z)
{

	if (z > laser->start)
	{
		// Ahead of laser
		return 0.0;
	} else if (z > laser->start - laser->rise)
	{
		// Laser rise
		t_fld csi = z - laser->start;
		t_fld e = sin(M_PI_2 * csi / laser->rise);
		return e * e;
	} else if (z > laser->start - (laser->rise + laser->flat))
	{
		// Flat-top
		return 1.0;
	} else if (z > laser->start - (laser->rise + laser->flat + laser->fall))
	{
		// Laser fall
		t_fld csi = z - (laser->start - laser->rise - laser->flat - laser->fall);
		t_fld e = sin(M_PI_2 * csi / laser->fall);
		return e * e;
	}

	// Before laser
	return 0.0;
}

void div_corr_x(t_emf *emf)
{
	int i, j;

	double ex, bx;

	t_vfld *restrict E = emf->E;
	t_vfld *restrict B = emf->B;
	const int nrow = emf->nrow;
	const double dx_dy = emf->dx[0] / emf->dx[1];

	for (j = 0; j < emf->nx[1]; j++)
	{
		ex = 0.0;
		bx = 0.0;
		for (i = emf->nx[0] - 1; i >= 0; i--)
		{
			ex += dx_dy * (E[i + 1 + j * nrow].y - E[i + 1 + (j - 1) * nrow].y);
			E[i + j * nrow].x = ex;

			bx += dx_dy * (B[i + (j + 1) * nrow].y - B[i + j * nrow].y);
			B[i + j * nrow].x = bx;
		}
	}
}

void emf_add_laser(t_emf *const emf, t_emf_laser *laser, int offset_y)
{
	// Validate laser parameters
	if (laser->fwhm != 0)
	{
		if (laser->fwhm <= 0)
		{
			fprintf(stderr, "Invalid laser FWHM, must be > 0, aborting.\n");
			exit(-1);
		}
		// The fwhm parameter overrides the rise/flat/fall parameters
		laser->rise = laser->fwhm;
		laser->fall = laser->fwhm;
		laser->flat = 0.;
	}

	if (laser->rise <= 0)
	{
		fprintf(stderr, "Invalid laser RISE, must be > 0, aborting.\n");
		exit(-1);
	}

	if (laser->flat < 0)
	{
		fprintf(stderr, "Invalid laser FLAT, must be >= 0, aborting.\n");
		exit(-1);
	}

	if (laser->fall <= 0)
	{
		fprintf(stderr, "Invalid laser FALL, must be > 0, aborting.\n");
		exit(-1);
	}

	// Launch laser
	int i, j, nrow;

	t_fld r_center, z, z_2, r, r_2;
	t_fld amp, lenv, lenv_2, k;
	t_fld dx, dy;
	t_fld cos_pol, sin_pol;

	t_vfld *restrict E = emf->E;
	t_vfld *restrict B = emf->B;

	nrow = emf->nrow;
	dx = emf->dx[0];
	dy = emf->dx[1];

	r_center = laser->axis;
	amp = laser->omega0 * laser->a0;

	cos_pol = cos(laser->polarization);
	sin_pol = sin(laser->polarization);

	switch (laser->type)
	{
		case PLANE:
			k = laser->omega0;

			for (i = 0; i < emf->nx[0]; i++)
			{
				z = i * dx;
				z_2 = z + dx / 2;

				lenv = amp * lon_env(laser, z);
				lenv_2 = amp * lon_env(laser, z_2);

				for (j = 0; j < emf->nx[1]; j++)
				{
					// E[i + j*nrow].x += 0.0
					E[i + j * nrow].y += +lenv * cos(k * z) * cos_pol;
					E[i + j * nrow].z += +lenv * cos(k * z) * sin_pol;

					// E[i + j*nrow].x += 0.0
					B[i + j * nrow].y += -lenv_2 * cos(k * z_2) * sin_pol;
					B[i + j * nrow].z += +lenv_2 * cos(k * z_2) * cos_pol;
				}
			}
			break;

		case GAUSSIAN:

			for (i = 0; i < emf->nx[0]; i++)
			{
				z = i * dx;
				z_2 = z + dx / 2;

				lenv = amp * lon_env(laser, z);
				lenv_2 = amp * lon_env(laser, z_2);

				for (j = 0; j < emf->nx[1]; j++)
				{
					r = (j + offset_y) * dy - r_center;
					r_2 = r + dy / 2;

					// E[i + j*nrow].x += 0.0
					E[i + j * nrow].y += +lenv * gauss_phase(laser, z, r_2) * cos_pol;
					E[i + j * nrow].z += +lenv * gauss_phase(laser, z, r) * sin_pol;

					// B[i + j*nrow].x += 0.0
					B[i + j * nrow].y += -lenv_2 * gauss_phase(laser, z_2, r) * sin_pol;
					B[i + j * nrow].z += +lenv_2 * gauss_phase(laser, z_2, r_2) * cos_pol;

				}
			}
			break;
		default:
			break;
	}
}

// Update the ghost cells in the X direction (CPU)
void emf_update_gc_x(t_emf *emf)
{
	int i, j;
	const int nrow = emf->nrow;

	t_vfld *const restrict E = emf->E;
	t_vfld *const restrict B = emf->B;

	// For moving window don't update x boundaries
	if (!emf->moving_window)
	{
		// x
		for (j = -emf->gc[1][0]; j < emf->nx[1] + emf->gc[1][1]; j++)
		{

			// lower
			for (i = -emf->gc[0][0]; i < 0; i++)
			{
				E[i + j * nrow].x = E[emf->nx[0] + i + j * nrow].x;
				E[i + j * nrow].y = E[emf->nx[0] + i + j * nrow].y;
				E[i + j * nrow].z = E[emf->nx[0] + i + j * nrow].z;

				B[i + j * nrow].x = B[emf->nx[0] + i + j * nrow].x;
				B[i + j * nrow].y = B[emf->nx[0] + i + j * nrow].y;
				B[i + j * nrow].z = B[emf->nx[0] + i + j * nrow].z;
			}

			// below
			for (i = 0; i < emf->gc[0][1]; i++)
			{
				E[emf->nx[0] + i + j * nrow].x = E[i + j * nrow].x;
				E[emf->nx[0] + i + j * nrow].y = E[i + j * nrow].y;
				E[emf->nx[0] + i + j * nrow].z = E[i + j * nrow].z;

				B[emf->nx[0] + i + j * nrow].x = B[i + j * nrow].x;
				B[emf->nx[0] + i + j * nrow].y = B[i + j * nrow].y;
				B[emf->nx[0] + i + j * nrow].z = B[i + j * nrow].z;
			}

		}
	}
}

// Update ghost cells in the below overlap zone (Y direction, CPU)
void emf_update_gc_y(t_emf *emf)
{
	int i, j;
	const int nrow = emf->nrow;

	t_vfld *const restrict E = emf->E;
	t_vfld *const restrict B = emf->B;
	t_vfld *const restrict E_overlap = emf->E_below;
	t_vfld *const restrict B_overlap = emf->B_below;

	// y
	for (i = -emf->gc[0][0]; i < emf->nx[0] + emf->gc[0][1]; i++)
	{
		for (j = -emf->gc[1][0]; j < 0; j++)
		{
			B[i + j * nrow] = B_overlap[i + (j + emf->gc[1][0]) * nrow];
			E[i + j * nrow] = E_overlap[i + (j + emf->gc[1][0]) * nrow];
		}

		for (j = 0; j < emf->gc[1][1]; j++)
		{
			B_overlap[i + (j + emf->gc[1][0]) * nrow] = B[i + j * nrow];
			E_overlap[i + (j + emf->gc[1][0]) * nrow] = E[i + j * nrow];
		}
	}
}

/*********************************************************************************************
 Diagnostics
 *********************************************************************************************/
// Reconstruct the simulation grid from all regions (eletric/magnetic field for a given direction)
void emf_reconstruct_global_buffer(const t_emf *emf, float *global_buffer, const int offset,
								   enum emf_field_type field, const char fc)
{
	t_vfld *restrict f;

	switch (field)
	{
		case EFLD:
			f = emf->E;
			break;
		case BFLD:
			f = emf->B;
			break;
		default:
			fprintf(stderr, "Invalid field type selected, returning\n");
			return;
			break;
	}

	float *restrict p = global_buffer + offset * emf->nx[0];

	switch (fc)
	{
		case 0:
			for (int j = 0; j < emf->nx[1]; j++)
			{
				for (int i = 0; i < emf->nx[0]; i++)
				{
					p[i] = f[i].x;
				}
				p += emf->nx[0];
				f += emf->nrow;
			}
			break;
		case 1:
			for (int j = 0; j < emf->nx[1]; j++)
			{
				for (int i = 0; i < emf->nx[0]; i++)
				{
					p[i] = f[i].y;
				}
				p += emf->nx[0];
				f += emf->nrow;
			}
			break;
		case 2:
			for (int j = 0; j < emf->nx[1]; j++)
			{
				for (int i = 0; i < emf->nx[0]; i++)
				{
					p[i] = f[i].z;
				}
				p += emf->nx[0];
				f += emf->nrow;
			}
			break;
		default:
			fprintf(stderr, "Invalid field component selected, returning\n");
			return;
	}
}

// Save the reconstructed buffer in a ZDF file
void emf_report(const float *restrict global_buffer, const float box[2], const int true_nx[2],
				const int iter, const float dt, const char field, const char fc,
				const char path[128])
{
	char vfname[3];

	// Choose field to save
	switch (field)
	{
		case EFLD:
			vfname[0] = 'E';
			break;
		case BFLD:
			vfname[0] = 'B';
			break;
		default:
			fprintf(stderr, "Invalid field type selected, returning\n");
			return;
	}

	switch (fc)
	{
		case 0:
			vfname[1] = '1';
			break;
		case 1:
			vfname[1] = '2';
			break;
		case 2:
			vfname[1] = '3';
			break;
		default:
			fprintf(stderr, "Invalid field component selected, returning\n");
			return;
	}
	vfname[2] = 0;

	t_zdf_grid_axis axis[2];
	axis[0] = (t_zdf_grid_axis ) { .min = 0.0, .max = box[0], .label = "x_1", .units = "c/\\omega_p" };

	axis[1] = (t_zdf_grid_axis ) { .min = 0.0, .max = box[1], .label = "x_2", .units = "c/\\omega_p" };

	t_zdf_grid_info info = { .ndims = 2, .label = vfname, .units = "m_e c \\omega_p e^{-1}",
			.axis = axis };

	info.nx[0] = true_nx[0];
	info.nx[1] = true_nx[1];

	t_zdf_iteration iteration = { .n = iter, .t = iter * dt, .time_units = "1/\\omega_p" };

	zdf_save_grid(global_buffer, &info, &iteration, path);

}

// Calculate the EMF energy
double emf_get_energy(t_emf *emf)
{
	t_vfld *const restrict E = emf->E;
	t_vfld *const restrict B = emf->B;
	double result = 0;

	for (unsigned int i = 0; i < emf->nx[0] * emf->nx[1]; i++)
	{
		result += 2 * E[i].x * E[i].x;
		result += E[i].y * E[i].y;
		result += E[i].z * E[i].z;
		result += B[i].x * B[i].x;
		result += B[i].y * B[i].y;
		result += B[i].z * B[i].z;
	}

	return result * 0.5 * emf->dx[0] * emf->dx[1];
}

/*********************************************************************************************
 Field solver
 *********************************************************************************************/

void yee_b_openacc(t_vfld *restrict B, const t_vfld *restrict E, const t_fld dt_dx,
				   const t_fld dt_dy, const int nrow, const int nx[2], const int queue)
{
	// Canonical implementation
	#pragma acc parallel loop independent tile(16, 16) async(queue)
	for (int j = -1; j <= nx[1]; j++)
	{
		for (int i = -1; i <= nx[0]; i++)
		{
			B[i + j * nrow].x += (-dt_dy * (E[i + (j + 1) * nrow].z - E[i + j * nrow].z));
			B[i + j * nrow].y += (dt_dx * (E[(i + 1) + j * nrow].z - E[i + j * nrow].z));
			B[i + j * nrow].z += (-dt_dx * (E[(i + 1) + j * nrow].y - E[i + j * nrow].y)
					+ dt_dy * (E[i + (j + 1) * nrow].x - E[i + j * nrow].x));
		}
	}
}

void yee_e_openacc(const t_vfld *restrict B, t_vfld *restrict E, const t_vfld *restrict J,
				   const const t_fld dt_dx, const t_fld dt_dy, const float dt, const int nrow_e,
				   const int nrow_j, const int nx[2], const int queue)
{
	// Canonical implementation
	#pragma acc parallel loop independent tile(16, 16) async(queue)
	for (int j = 0; j <= nx[1] + 1; j++)
	{
		for (int i = 0; i <= nx[0]; i++)
		{
			E[i + j * nrow_e].x += (+dt_dy * (B[i + j * nrow_e].z - B[i + (j - 1) * nrow_e].z))
					- dt * J[i + j * nrow_j].x;
			E[i + j * nrow_e].y += (-dt_dx * (B[i + j * nrow_e].z - B[(i - 1) + j * nrow_e].z))
					- dt * J[i + j * nrow_j].y;
			E[i + j * nrow_e].z += (+dt_dx * (B[i + j * nrow_e].y - B[(i - 1) + j * nrow_e].y)
					- dt_dy * (B[i + j * nrow_e].x - B[i + (j - 1) * nrow_e].x)) - dt * J[i + j * nrow_j].z;
		}
	}
}

// Update the ghost cells in the X direction (OpenAcc)
void emf_update_gc_x_openacc(t_vfld *restrict E, t_vfld *restrict B, const int nrow, const int nx[2],
							 const int gc[2][2], const int queue)
{
	#pragma acc parallel loop collapse(2) independent async(queue)
	for (int j = -gc[1][0]; j < nx[1] + gc[1][1]; j++)
	{
		for (int i = -gc[0][0]; i < gc[0][1]; i++)
		{
			if (i < 0)
			{
				E[i + j * nrow].x = E[nx[0] + i + j * nrow].x;
				E[i + j * nrow].y = E[nx[0] + i + j * nrow].y;
				E[i + j * nrow].z = E[nx[0] + i + j * nrow].z;

				B[i + j * nrow].x = B[nx[0] + i + j * nrow].x;
				B[i + j * nrow].y = B[nx[0] + i + j * nrow].y;
				B[i + j * nrow].z = B[nx[0] + i + j * nrow].z;
			} else
			{
				E[nx[0] + i + j * nrow].x = E[i + j * nrow].x;
				E[nx[0] + i + j * nrow].y = E[i + j * nrow].y;
				E[nx[0] + i + j * nrow].z = E[i + j * nrow].z;

				B[nx[0] + i + j * nrow].x = B[i + j * nrow].x;
				B[nx[0] + i + j * nrow].y = B[i + j * nrow].y;
				B[nx[0] + i + j * nrow].z = B[i + j * nrow].z;
			}
		}
	}
}

// Update ghost cells in the below overlap zone (Y direction, OpenAcc)
void emf_update_gc_y_openacc(t_emf *emf)
{
	const int nrow = emf->nrow;

	t_vfld *const restrict E = emf->E;
	t_vfld *const restrict B = emf->B;
	t_vfld *const restrict E_overlap = emf->E_below;
	t_vfld *const restrict B_overlap = emf->B_below;

	// y
	#pragma acc parallel loop collapse(2) independent
	for (int i = -emf->gc[0][0]; i < emf->nx[0] + emf->gc[0][1]; i++)
	{
		for (int j = -emf->gc[1][0]; j < emf->gc[1][1]; j++)
		{
			if(j < 0)
			{
				B[i + j * nrow] = B_overlap[i + (j + emf->gc[1][0]) * nrow];
				E[i + j * nrow] = E_overlap[i + (j + emf->gc[1][0]) * nrow];
			}else
			{
				B_overlap[i + (j + emf->gc[1][0]) * nrow] = B[i + j * nrow];
				E_overlap[i + (j + emf->gc[1][0]) * nrow] = E[i + j * nrow];
			}
		}
	}
}

// Move the simulation window
void emf_move_window_openacc(t_vfld *restrict E, t_vfld *restrict B, const int nrow,
							 const int gc[2][2], const int nx[2], const int queue)
{
	const t_vfld zero_fld = {0, 0, 0};

	// Shift data left 1 cell and zero rightmost cells
	#pragma acc parallel loop gang vector_length(384) async(queue)
	for (int j = 0; j < gc[1][0] + nx[1] + gc[1][1]; j++)
	{
		t_vfld B_temp[LOCAL_BUFFER_SIZE];
		t_vfld E_temp[LOCAL_BUFFER_SIZE];

		#pragma acc cache(B_temp[0:LOCAL_BUFFER_SIZE])
		#pragma acc cache(E_temp[0:LOCAL_BUFFER_SIZE])

		for (int begin_idx = 0; begin_idx < nrow; begin_idx += LOCAL_BUFFER_SIZE)
		{
			#pragma acc loop vector
			for (int i = 0; i < LOCAL_BUFFER_SIZE; i++)
			{
				if ((begin_idx + i) < gc[0][0] + nx[0] - 1)
				{
					B_temp[i] = B[begin_idx + 1 + i + j * nrow];
					E_temp[i] = E[begin_idx + 1 + i + j * nrow];
				} else
				{
					B_temp[i] = zero_fld;
					E_temp[i] = zero_fld;
				}
			}

			#pragma acc loop vector
			for (int i = 0; i < LOCAL_BUFFER_SIZE; i++)
			{
				if (begin_idx + i < nrow)
				{
					E[begin_idx + i + j * nrow] = E_temp[i];
					B[begin_idx + i + j * nrow] = B_temp[i];
				}
			}
		}
	}
}

// Perform the local integration of the fields (and post processing). OpenAcc Task
void emf_advance_openacc(t_emf *emf, const t_current *current)
{
	const int queue = nanos6_get_current_acc_queue();

	const t_fld dt = emf->dt;
	const t_fld dt_dx = dt / emf->dx[0];
	const t_fld dt_dy = dt / emf->dx[1];

	emf->iter++;
	const bool shift = (emf->iter * dt) > emf->dx[0] * (emf->n_move + 1);

	// Advance EM field using Yee algorithm modified for having E and B time centered
	yee_b_openacc(emf->B, emf->E, dt_dx / 2.0f, dt_dy / 2.0f, emf->nrow, emf->nx, queue);
	yee_e_openacc(emf->B, emf->E, current->J, dt_dx, dt_dy, dt, emf->nrow, current->nrow, emf->nx, queue);
	yee_b_openacc(emf->B, emf->E, dt_dx / 2.0f, dt_dy / 2.0f, emf->nrow, emf->nx, queue);

	if(emf->moving_window)
	{
		if(shift)
		{
			emf->n_move++;

			// Move simulation window
			emf_move_window_openacc(emf->E_buf, emf->B_buf, emf->nrow, emf->gc, emf->nx, queue);
		}
	} else
	{
		// Update guard cells with new values
		emf_update_gc_x_openacc(emf->E, emf->B, emf->nrow, emf->nx, emf->gc, queue);
	}
}

