#include "emf.h"
#include <stdlib.h>

void yee_b_openacc(t_emf *emf, const float dt)
{
	// these must not be unsigned because we access negative cell indexes
	t_fld dt_dx, dt_dy;
	t_vfld *const restrict B = emf->B;
	const t_vfld *const restrict E = emf->E;
	const int nrow = emf->nrow;

	dt_dx = dt / emf->dx[0];
	dt_dy = dt / emf->dx[1];

	// Canonical implementation
	#pragma acc parallel loop independent tile(4, 4)
	for (int j = -1; j <= emf->nx[1]; j++)
	{
		for (int i = -1; i <= emf->nx[0]; i++)
		{
			B[i + j * nrow].x += (-dt_dy * (E[i + (j + 1) * nrow].z - E[i + j * nrow].z));
			B[i + j * nrow].y += (dt_dx * (E[(i + 1) + j * nrow].z - E[i + j * nrow].z));
			B[i + j * nrow].z += (-dt_dx * (E[(i + 1) + j * nrow].y - E[i + j * nrow].y)
					+ dt_dy * (E[i + (j + 1) * nrow].x - E[i + j * nrow].x));
		}
	}
}

void yee_e_openacc(t_emf *emf, const t_current *current, const float dt)
{
	// these must not be unsigned because we access negative cell indexes
	const int nrow_e = emf->nrow;
	const int nrow_j = current->nrow;

	t_fld dt_dx, dt_dy;

	dt_dx = dt / emf->dx[0];
	dt_dy = dt / emf->dx[1];

	t_vfld *const restrict E = emf->E;
	const t_vfld *const restrict B = emf->B;
	const t_vfld *const restrict J = current->J;

	// Canonical implementation
	#pragma acc parallel loop independent tile(4, 4)
	for (int j = 0; j <= emf->nx[1] + 1; j++)
	{
		for (int i = 0; i <= emf->nx[0] + 1; i++)
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

// This code operates with periodic boundaries
void emf_update_gc_openacc(t_emf *emf)
{
	const int nrow = emf->nrow;

	t_vfld *const restrict E = emf->E;
	t_vfld *const restrict B = emf->B;

	// For moving window don't update x boundaries
	if (!emf->moving_window)
	{
		// x
		#pragma acc parallel loop independent collapse(2)
		for (int j = -emf->gc[1][0]; j < emf->nx[1] + emf->gc[1][1]; j++)
		{
			for (int i = -emf->gc[0][0]; i < emf->gc[0][1]; i++)
			{
				if(i < 0)
				{
					E[i + j * nrow].x = E[emf->nx[0] + i + j * nrow].x;
					E[i + j * nrow].y = E[emf->nx[0] + i + j * nrow].y;
					E[i + j * nrow].z = E[emf->nx[0] + i + j * nrow].z;

					B[i + j * nrow].x = B[emf->nx[0] + i + j * nrow].x;
					B[i + j * nrow].y = B[emf->nx[0] + i + j * nrow].y;
					B[i + j * nrow].z = B[emf->nx[0] + i + j * nrow].z;
				}else
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

	// y
	#pragma acc parallel loop independent collapse(2)
	for (int i = -emf->gc[0][0]; i < emf->nx[0] + emf->gc[0][1]; i++)
	{
		for(int j = -emf->gc[1][0]; j < emf->gc[1][1]; j++)
		{
			if(j < 0)
			{
				E[i + j * nrow].x = E[i + (emf->nx[1] + j) * nrow].x;
				E[i + j * nrow].y = E[i + (emf->nx[1] + j) * nrow].y;
				E[i + j * nrow].z = E[i + (emf->nx[1] + j) * nrow].z;

				B[i + j * nrow].x = B[i + (emf->nx[1] + j) * nrow].x;
				B[i + j * nrow].y = B[i + (emf->nx[1] + j) * nrow].y;
				B[i + j * nrow].z = B[i + (emf->nx[1] + j) * nrow].z;
			}else
			{
				E[i + (emf->nx[1] + j) * nrow].x = E[i + j * nrow].x;
				E[i + (emf->nx[1] + j) * nrow].y = E[i + j * nrow].y;
				E[i + (emf->nx[1] + j) * nrow].z = E[i + j * nrow].z;

				B[i + (emf->nx[1] + j) * nrow].x = B[i + j * nrow].x;
				B[i + (emf->nx[1] + j) * nrow].y = B[i + j * nrow].y;
				B[i + (emf->nx[1] + j) * nrow].z = B[i + j * nrow].z;
			}
		}
	}
}

void emf_move_window_openacc(t_emf *emf)
{
	if ((emf->iter * emf->dt) > emf->dx[0] * (emf->n_move + 1))
	{
		const int nrow = emf->nrow;
		size_t size = emf->nrow * (emf->gc[1][0] + emf->gc[1][1] + emf->nx[1]);
		t_vfld *const restrict E = malloc(size * sizeof(t_vfld));
		t_vfld *const restrict B = malloc(size * sizeof(t_vfld));

		const t_vfld zero_fld = {0., 0., 0.};

		memcpy(B, emf->B_buf, size * sizeof(t_vfld));
		memcpy(E, emf->E_buf, size * sizeof(t_vfld));

		// Shift data left 1 cell and zero rightmost cells
		#pragma acc parallel loop independent
		for (int j = 0; j < emf->gc[1][0] + emf->nx[1] + emf->gc[1][1]; j++)
		{
			for (int i = 0; i < emf->nrow; i++)
			{
				if (i < emf->gc[0][0] + emf->nx[0] - 1)
				{
					emf->E_buf[i + j * nrow] = E[i + j * nrow + 1];
					emf->B_buf[i + j * nrow] = B[i + j * nrow + 1];
				} else
				{
					emf->E_buf[i + j * nrow] = zero_fld;
					emf->B_buf[i + j * nrow] = zero_fld;
				}
			}
		}

		// Increase moving window counter
		emf->n_move++;

		free(E);
		free(B);
	}
}

void emf_advance_openacc(t_emf *emf, const t_current *current)
{
	const float dt = emf->dt;

	// Advance EM field using Yee algorithm modified for having E and B time centered
	yee_b_openacc(emf, dt / 2.0f);
	yee_e_openacc(emf, current, dt);
	yee_b_openacc(emf, dt / 2.0f);

	// Update guard cells with new values
	emf_update_gc_openacc(emf);

	// Advance internal iteration number
	emf->iter += 1;

	// Move simulation window if needed
	if (emf->moving_window) emf_move_window_openacc(emf);
}