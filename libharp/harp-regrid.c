/*
 * Copyright (C) 2015-2017 S[&]T, The Netherlands.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "harp-internal.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum resample_type_enum
{
    resample_skip,
    resample_remove,
    resample_linear,
    resample_log,
    resample_interval
} resample_type;


static long get_unpadded_length(double *vector, long vector_length)
{
    long i;

    for (i = vector_length - 1; i >= 0; i--)
    {
        if (!harp_isnan(vector[i]))
        {
            return i + 1;
        }
    }

    return vector_length;
}

static resample_type get_resample_type(harp_variable *variable, harp_dimension_type dimension_type)
{
    int num_matching_dims;
    int i;

    /* ensure that there is only 1 dimension of the given type */
    for (i = 0, num_matching_dims = 0; i < variable->num_dimensions; i++)
    {
        if (variable->dimension_type[i] == dimension_type)
        {
            num_matching_dims++;
        }
    }

    if (num_matching_dims == 0)
    {
        /* if the variable has no matching dimension, we should always skip */
        return resample_skip;
    }

    if (num_matching_dims != 1)
    {
        /* remove all variables with more than one matching dimension */
        /* TODO: how to resample 2D AVKs */
        return resample_remove;
    }

    /* we can't resample strings */
    if (variable->data_type == harp_type_string)
    {
        return resample_remove;
    }

    /* uncertainty propagation needs to be handled differently (remove for now) */
    if (strstr(variable->name, "_uncertainty") != NULL)
    {
        return resample_remove;
    }

    /* boundary variables needs to be handled differently (remove for now) */
    if (strstr(variable->name, "_bounds") != NULL)
    {
        return resample_remove;
    }

    if (dimension_type == harp_dimension_vertical)
    {
        /* TODO: how to resample 1D column AVKs */
        if (strstr(variable->name, "_avk") != NULL)
        {
            return resample_remove;
        }
        /* use interval interpolation for vertical regridding of partial column profiles */
        if (strstr(variable->name, "_column_") != NULL)
        {
            return resample_interval;
        }
    }

    /* resample linearly by default */
    return resample_linear;
}

static int needs_interval_resample(harp_product *product, harp_dimension_type dimension_type)
{
    int i;

    for (i = 0; i < product->num_variables; i++)
    {
        if (get_resample_type(product->variable[i], dimension_type) == resample_interval)
        {
            return 1;
        }
    }

    return 0;
}

static int resize_dimension(harp_product *product, harp_dimension_type dimension_type, long num_elements)
{
    int i;

    for (i = 0; i < product->num_variables; i++)
    {
        harp_variable *var = product->variable[i];
        int j;

        for (j = 0; j < var->num_dimensions; j++)
        {
            if (var->dimension_type[j] == dimension_type)
            {
                if (harp_variable_resize_dimension(var, j, num_elements) != 0)
                {
                    return -1;
                }
            }
        }
    }

    product->dimension[dimension_type] = num_elements;

    return 0;
}


static int filter_resamplable_variables(harp_product *product, harp_dimension_type dimension_type)
{
    int i;

    for (i = product->num_variables - 1; i >= 0; i--)
    {
        if (get_resample_type(product->variable[i], dimension_type) == resample_remove)
        {
            if (harp_product_remove_variable(product, product->variable[i]) != 0)
            {
                return -1;
            }
        }
    }

    return 0;
}

int harp_product_get_derived_bounds_for_grid(harp_product *product, harp_variable *grid, harp_variable **bounds)
{
    harp_dimension_type dim_type[HARP_MAX_NUM_DIMS];
    char *bounds_name = NULL;
    int i;

    assert(grid->num_dimensions < HARP_MAX_NUM_DIMS);
    for (i = 0; i < grid->num_dimensions; i++)
    {
        dim_type[i] = grid->dimension_type[i];
    }
    dim_type[grid->num_dimensions] = harp_dimension_independent;

    /* derive the name of the bounds variable for the vertical axis */
    bounds_name = malloc(strlen(grid->name) + 7 + 1);
    if (!bounds_name)
    {
        harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not duplicate string)"
                       " (%s:%u)", __FILE__, __LINE__);
        return -1;
    }
    strcpy(bounds_name, grid->name);
    strcat(bounds_name, "_bounds");

    if (harp_product_get_derived_variable(product, bounds_name, grid->unit, grid->num_dimensions + 1, dim_type, bounds)
        != 0)
    {
        free(bounds_name);
        return -1;
    }
    free(bounds_name);

    return 0;
}

static int get_bounds_for_grid_from_variable(harp_variable *grid, harp_variable **bounds)
{
    harp_product *product = NULL;

    /* Create a dummy product to allow deriving the bounds for the target grid */
    if (harp_product_new(&product) != 0)
    {
        return -1;
    }
    if (harp_product_add_variable(product, grid) != 0)
    {
        harp_product_delete(product);
        return -1;
    }
    if (harp_product_get_derived_bounds_for_grid(product, grid, bounds) != 0)
    {
        if (harp_product_detach_variable(product, grid) == 0)
        {
            harp_product_delete(product);
        }
        return -1;
    }
    if (harp_product_detach_variable(product, grid) != 0)
    {
        /* we can't delete the product since it still contains the grid (which we can't delete) */
        return -1;
    }
    harp_product_delete(product);

    return 0;
}

/** \addtogroup harp_product
 * @{
 */

/**
 * Resample all variables in product against a specified grid.
 * The target grid variable should be an axis variable containing the target grid (as 'double' values).
 * It should be a one-dimensional variable (for a time independent grid) or a two-dimensional variable
 * (for a time dependent grid).
 * The dimension to use for regridding is based on the type of the last dimenion of the target grid variable.
 * This function cannot be used to regrid the time dimension (or an independent dimension).
 *
 * If the target grid variable is two dimensional (i.e. time dependent) then its time dimension should match that of 
 * the product.
 *
 * For each variable in the product a dimension-specific rule based on the variable name will determine how to regrid
 * the variable (point/interval interpolation).
 * If interval interpolation is needed for one of the variables then target boundaries are needed.
 * These can be provided using the optional target_bounds parameter. If this parameter is not provided, the boundaries
 * will be calculated automatically from the target grid (by inter/extrapolating intervals from mid-points).
 *
 * The source grid (and bounds) are determined by performing a variable derivation on the product (using the variable
 * name of the target_grid variable).
 *
 * \param product Product to resample.
 * \param target_grid Target grid variable.
 * \param target_bounds Target grid boundaries variable (optional).
 *
 * \return
 *   \arg \c 0, Success.
 *   \arg \c -1, Error occurred (check #harp_errno).
 */
LIBHARP_API int harp_product_regrid_with_axis_variable(harp_product *product, harp_variable *target_grid,
                                                       harp_variable *target_bounds)
{
    harp_dimension_type grid_dim_type[2];
    harp_dimension_type dimension_type;
    long source_max_dim_elements;       /* actual elems + NaN padding */
    long source_grid_max_dim_elements;
    long source_grid_num_dim_elements;
    long target_grid_max_dim_elements;
    long target_grid_num_dim_elements;
    long source_num_time_elements = 1;
    int source_grid_num_dims = 1;
    int target_grid_num_dims;
    long i;

    /* owned memory */
    harp_variable *source_grid = NULL;
    harp_variable *source_bounds = NULL;
    harp_variable *local_target_grid = NULL;
    harp_variable *local_target_bounds = NULL;
    double *source_buffer = NULL;
    double *target_buffer = NULL;

    target_grid_num_dims = target_grid->num_dimensions;
    if (target_grid_num_dims != 1 && target_grid_num_dims != 2)
    {
        harp_set_error(HARP_ERROR_INVALID_ARGUMENT, "invalid dimensions for axis variable");
        return -1;
    }
    dimension_type = target_grid->dimension_type[target_grid->num_dimensions - 1];
    if (dimension_type == harp_dimension_time || dimension_type == harp_dimension_independent)
    {
        harp_set_error(HARP_ERROR_INVALID_ARGUMENT, "invalid dimensions for axis variable");
        return -1;
    }
    if (target_grid_num_dims == 2)
    {
        if (target_grid->dimension_type[0] != harp_dimension_time)
        {
            harp_set_error(HARP_ERROR_INVALID_ARGUMENT, "invalid dimensions for axis variable");
            return -1;
        }
        if (target_grid->dimension[0] != product->dimension[harp_dimension_time])
        {
            harp_set_error(HARP_ERROR_INVALID_ARGUMENT, "time dimension of axis variable does not match product");
            return -1;
        }
    }
    target_grid_max_dim_elements = target_grid->dimension[target_grid_num_dims - 1];

    if (harp_variable_copy(target_grid, &local_target_grid) != 0)
    {
        goto error;
    }

    if (target_bounds != NULL)
    {
        if (target_bounds->num_dimensions != target_grid_num_dims + 1)
        {
            harp_set_error(HARP_ERROR_INVALID_ARGUMENT, "inconsistent dimensions for axis bounds variable");
            return -1;
        }
        if ((target_bounds->dimension_type[0] != target_grid->dimension_type[0]) ||
            (target_bounds->dimension[0] != target_grid->dimension[0]))
        {
            harp_set_error(HARP_ERROR_INVALID_ARGUMENT, "inconsistent dimensions for axis bounds variable");
            return -1;
        }
        if (target_grid_num_dims == 2)
        {
            if ((target_bounds->dimension_type[1] != target_grid->dimension_type[1]) ||
                (target_bounds->dimension[1] != target_grid->dimension[1]))
            {
                harp_set_error(HARP_ERROR_INVALID_ARGUMENT, "inconsistent dimensions for axis bounds variable");
                return -1;
            }
        }
        if (target_bounds->dimension_type[target_grid_num_dims] != harp_dimension_independent ||
            target_bounds->dimension[target_grid_num_dims] != 2)
        {
            harp_set_error(HARP_ERROR_INVALID_ARGUMENT, "invalid independent dimension for axis bounds variable");
            return -1;
        }

        if (harp_variable_copy(target_bounds, &local_target_bounds) != 0)
        {
            goto error;
        }
    }

    grid_dim_type[0] = harp_dimension_time;
    grid_dim_type[1] = dimension_type;

    /* Derive the source grid (will give doubles because unit is passed) */
    /* Try time independent */
    if (harp_product_get_derived_variable(product, target_grid->name, target_grid->unit, 1, &grid_dim_type[1],
                                          &source_grid) != 0)
    {
        /* Failed to derive time independent. Try time dependent. */
        if (harp_product_get_derived_variable(product, target_grid->name, target_grid->unit, 2, grid_dim_type,
                                              &source_grid) != 0)
        {
            goto error;
        }
        source_grid_num_dims = 2;
        source_num_time_elements = source_grid->dimension[0];
    }
    source_grid_max_dim_elements = source_grid->dimension[source_grid->num_dimensions - 1];
    source_max_dim_elements = source_grid_max_dim_elements;

    /* derive bounds variables if necessary for resampling */
    if (needs_interval_resample(product, dimension_type))
    {
        if (local_target_bounds == NULL)
        {
            if (get_bounds_for_grid_from_variable(local_target_grid, &local_target_bounds) != 0)
            {
                goto error;
            }
        }
        if (harp_product_get_derived_bounds_for_grid(product, source_grid, &source_bounds) != 0)
        {
            goto error;
        }
    }

    /* remove grid variables if they exists (since we don't want to interpolate them) */
    /* this won't affect the source_grid/source_bounds variables that we already derived */
    if (harp_product_has_variable(product, source_grid->name))
    {
        if (harp_product_remove_variable_by_name(product, source_grid->name) != 0)
        {
            goto error;
        }
    }
    if (source_bounds != NULL)
    {
        if (harp_product_has_variable(product, source_bounds->name))
        {
            if (harp_product_remove_variable_by_name(product, source_bounds->name) != 0)
            {
                goto error;
            }
        }
    }

    /* remove variables that can't be resampled */
    if (filter_resamplable_variables(product, dimension_type) != 0)
    {
        goto error;
    }

    /* Use loglin interpolation if vertical pressure grid */
    if (dimension_type == harp_dimension_vertical && strcmp(local_target_grid->name, "pressure") == 0)
    {
        for (i = 0; i < source_grid->num_elements; i++)
        {
            source_grid->data.double_data[i] = log(source_grid->data.double_data[i]);
        }
        for (i = 0; i < local_target_grid->num_elements; i++)
        {
            local_target_grid->data.double_data[i] = log(local_target_grid->data.double_data[i]);
        }
        if (source_bounds != NULL)
        {
            for (i = 0; i < source_bounds->num_elements; i++)
            {
                source_bounds->data.double_data[i] = log(source_bounds->data.double_data[i]);
            }
        }
        if (local_target_bounds != NULL)
        {
            for (i = 0; i < local_target_bounds->num_elements; i++)
            {
                local_target_bounds->data.double_data[i] = log(local_target_bounds->data.double_data[i]);
            }
        }
    }

    /* Resize the dimension in the target product to make room for the resampled data */
    if (target_grid_max_dim_elements > source_max_dim_elements)
    {
        if (resize_dimension(product, dimension_type, target_grid_max_dim_elements) != 0)
        {
            goto error;
        }
        source_max_dim_elements = target_grid_max_dim_elements;
    }

    /* allocate the buffers for the interpolation */
    source_buffer = (double *)malloc(source_max_dim_elements * (size_t)sizeof(double));
    if (source_buffer == NULL)
    {
        harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not allocate %lu bytes) (%s:%u)",
                       source_max_dim_elements * sizeof(double), __FILE__, __LINE__);
        goto error;
    }
    target_buffer = (double *)malloc(target_grid_max_dim_elements * (size_t)sizeof(double));
    if (target_buffer == NULL)
    {
        harp_set_error(HARP_ERROR_OUT_OF_MEMORY, "out of memory (could not allocate %lu bytes) (%s:%u)",
                       target_grid_max_dim_elements * sizeof(double), __FILE__, __LINE__);
        goto error;
    }

    /* regrid each variable */
    for (i = product->num_variables - 1; i >= 0; i--)
    {
        harp_variable *variable = product->variable[i];
        resample_type type;
        long source_time_index;
        long target_time_index;
        long num_blocks;
        long num_elements;
        long j;

        /* Check if we can resample this kind of variable */
        type = get_resample_type(variable, dimension_type);

        assert(type != resample_remove);
        if (type == resample_skip)
        {
            continue;
        }

        /* Ensure that the variable data consists of doubles */
        if (variable->data_type != harp_type_double && harp_variable_convert_data_type(variable, harp_type_double) != 0)
        {
            goto error;
        }

        /* Make time independent variables time dependent if source grid is time dependent */
        if (source_grid_num_dims > 1)
        {
            if (variable->dimension_type[0] != harp_dimension_time)
            {
                if (harp_variable_add_dimension(variable, 0, harp_dimension_time, source_num_time_elements) != 0)
                {
                    return -1;
                }
            }
        }

        /* treat variable as a [num_blocks, source_max_dim_elements, num_elements] array with indices [j,k,l] */
        num_blocks = 1;
        num_elements = 1;
        j = 0;
        assert(variable->num_dimensions > 0);
        while (variable->dimension_type[j] != dimension_type)
        {
            assert(j < variable->num_dimensions - 1);
            num_blocks *= variable->dimension[j];
            j++;
        }
        j++;    /* skip dimension that is going to be regridded */
        while (j < variable->num_dimensions)
        {
            num_elements *= variable->dimension[j];
            j++;
        }

        /* interpolate the data of the variable over the given dimension */
        /* keep track of time index separately since num_blocks can capture more than just the time dimension */
        source_time_index = 0;
        target_time_index = 0;
        for (j = 0; j < num_blocks; j++)
        {
            long k, l;

            /* keep track of time index for time dependent grids */
            if (j % source_num_time_elements == 0)
            {
                if (source_grid_num_dims == 2 && j > 0)
                {
                    source_time_index++;
                }
                /* find the actual grid lengths */
                source_grid_num_dim_elements =
                    get_unpadded_length(&source_grid->data.double_data[source_time_index *
                                                                       source_grid_max_dim_elements],
                                        source_grid_max_dim_elements);
                if (target_grid_num_dims == 2 || j == 0)
                {
                    /* if target grid is time dependent, then source grid will also be time dependent */
                    target_time_index = source_time_index;
                    target_grid_num_dim_elements =
                        get_unpadded_length(&target_grid->data.double_data[target_time_index *
                                                                           target_grid_max_dim_elements],
                                            target_grid_max_dim_elements);
                }
            }

            for (l = 0; l < num_elements; l++)
            {
                /* we need to regrid by tacking a slice for each sub element 'l' */
                for (k = 0; k < source_grid_num_dim_elements; k++)
                {
                    source_buffer[k] = variable->data.double_data[(j * source_max_dim_elements + k) * num_elements + l];
                }
                if (type == resample_linear)
                {
                    harp_interpolate_array_linear
                        (source_grid_num_dim_elements,
                         &source_grid->data.double_data[source_time_index * source_grid_max_dim_elements],
                         source_buffer, target_grid_num_dim_elements,
                         &local_target_grid->data.double_data[target_time_index * target_grid_max_dim_elements], 0,
                         target_buffer);
                }
                else if (type == resample_interval)
                {
                    harp_interval_interpolate_array_linear
                        (source_grid_num_dim_elements,
                         &source_bounds->data.double_data[source_time_index * source_grid_max_dim_elements * 2],
                         source_buffer, target_grid_num_dim_elements,
                         &local_target_bounds->data.double_data[target_time_index * target_grid_max_dim_elements * 2],
                         target_buffer);
                }
                else
                {
                    /* other resampling methods are not supported, but should also never be set */
                    assert(0);
                    exit(1);
                }

                for (k = 0; k < target_grid_num_dim_elements; k++)
                {
                    variable->data.double_data[(j * source_max_dim_elements + k) * num_elements + l] = target_buffer[k];
                }
                for (k = target_grid_num_dim_elements; k < target_grid_max_dim_elements; k++)
                {
                    variable->data.double_data[(j * source_max_dim_elements + k) * num_elements + l] = harp_nan();
                }
            }
        }
    }

    /* Resize the vertical dimension in the target product to minimal size */
    if (target_grid_max_dim_elements < source_max_dim_elements)
    {
        if (resize_dimension(product, dimension_type, target_grid_max_dim_elements) != 0)
        {
            goto error;
        }
    }

    /* ensure consistent axis variables in product */
    if (harp_product_add_variable(product, local_target_grid) != 0)
    {
        goto error;
    }
    local_target_grid = NULL;
    if (local_target_bounds != NULL)
    {
        if (harp_product_add_variable(product, local_target_bounds) != 0)
        {
            goto error;
        }
        local_target_bounds = NULL;
    }

    /* cleanup */
    harp_variable_delete(source_grid);
    harp_variable_delete(source_bounds);
    harp_variable_delete(local_target_grid);
    harp_variable_delete(local_target_bounds);
    free(source_buffer);
    free(target_buffer);

    return 0;

  error:
    harp_variable_delete(source_grid);
    harp_variable_delete(source_bounds);
    harp_variable_delete(local_target_grid);
    harp_variable_delete(local_target_bounds);
    free(source_buffer);
    free(target_buffer);

    return -1;
}

/**
 * @}
 */