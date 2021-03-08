/*
  This file is part of t8code.
  t8code is a C library to manage a collection (a forest) of multiple
  connected adaptive space-trees of general element classes in parallel.

  Copyright (C) 2015 the developers

  t8code is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  t8code is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with t8code; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#include <t8.h>
#include <t8_forest.h>
#include <t8_forest_vtk.h>
#include <t8_forest/t8_forest_iterate.h>
#include <t8_schemes/t8_default/t8_default_quad_cxx.hxx>
#include "t8_latlon_refine.h"
#include "t8_latlon_data.h"
#include "t8_messy_coupler.h"

inline 
double get_mean(double* values, int num_elements) {
  double mean_value = 0.0;
  int i;
  for (i=0; i < num_elements; ++i) {
    mean_value += values[i];
  }
  mean_value /= num_elements;
  return mean_value;
}

inline 
double get_max(double* values, int num_elements) {
  double max_value = LONG_MIN;
  int i;
  for (i=0; i < num_elements; ++i) {
    max_value = fmax(max_value, values[i]);
  }
  return max_value;
}

inline 
double get_min(double* values, int num_elements) {
  double min_value = LONG_MAX;
  int i;
  for (i=0; i < num_elements; ++i) {
    min_value = fmin(min_value, values[i]);
  }
  return min_value;
}

inline
void get_values(int first, int num_elements, int element_length, int tracer, double* values, double* data) {
  int offset, i;
  double value;
  for (i=0; i < num_elements; ++i) {
    offset = first + i * element_length + tracer;
    values[i] = data[offset];
  }
}

double mult_sum(int num_elements, double* a, double* b, double missing_value) {
  double sum = 0.0;
  for(int i=0; i<num_elements; ++i) {
    if(a[i] == missing_value && b[i] == missing_value) continue;
    sum += (a[i] * b[i]);
  }
  return sum;
}

double sum(int num_elements, double* a, double missing_value) {
  double sum = 0.0;
  for(int i=0; i<num_elements; ++i) {
    if(a[i] == missing_value) continue;
    sum = sum + a[i];
  }
  return sum;
}

void calculate_errors(int num_elements, double* values, double* errors, double value, double missing_value) {
  for(int i=0; i < num_elements; ++i) {
    errors[i] = values[i] == missing_value ? 0 : fabs(values[i] - value);
  }
}

void calculate_error_ratios(int num_elements, double* values, double* errors, double value, double missing_value) {
  for(int i=0; i < num_elements; ++i) {
    if(values[i] == missing_value || values[i] == 0) errors[i] = 0;
    else {
      errors[i] =  fabs(values[i] - value) / values[i];
    }
  }
}

int check_errors(int num_elements, double* errors, double max_error) {
  for(int i=0; i<num_elements; ++i) {
    if (errors[i] > max_error) {
      return 1;
    }
  }

  return 0;
}

int check_errors_by_ratio(int num_elements, double ratio, double* errors, double* values, double missing_value) {
  for(int i=0; i<num_elements; ++i) {
    if (values[i] == missing_value) continue;
    if (errors[i] > values[i] * ratio) {
      return 1;
    }
  }

  return 0;
}


t8_messy_custom_func_t* t8_messy_new_custom_func(int num_elements) {
  t8_messy_custom_func_t* func_data = T8_ALLOC(t8_messy_custom_func_t, 1);
  func_data->num_elements = num_elements;
  func_data->x_coords = T8_ALLOC_ZERO(int, num_elements);
  func_data->y_coords = T8_ALLOC_ZERO(int, num_elements);
  func_data->latitudes = T8_ALLOC_ZERO(double, num_elements);
  func_data->longitudes = T8_ALLOC_ZERO(double, num_elements);
  func_data->values = T8_ALLOC_ZERO(double, num_elements);
  func_data->tracer = T8_ALLOC(char, BUFSIZ);

  return func_data;
}

void t8_messy_destroy_custom_func(t8_messy_custom_func_t* custom) {
  T8_FREE(custom->x_coords);
  T8_FREE(custom->y_coords);
  T8_FREE(custom->latitudes);
  T8_FREE(custom->longitudes);
  T8_FREE(custom->values);
  T8_FREE(custom->tracer);
  T8_FREE(custom);
}

/**
 * Callback function determening weather 4 cells can be combined.
 * 
 * This callback function calculates the error that would be produced by interpolating
 * and only allowes if the generated error for every tracer is below a certain error tolerance.
 */
int
t8_messy_coarsen_by_error_tol_callback(t8_forest_t forest,
                          t8_forest_t forest_from,
                          int which_tree,
                          int lelement_id,
                          t8_eclass_scheme_c * ts,
                          int num_elements, t8_element_t * elements[]) 
{
  int ret = 0;

  /* since we don't want to refine, 
     we can stop if we only have one element */
  if (num_elements == 1) {
    return ret;
  }

  t8_messy_data_t *messy_data = (t8_messy_data_t*) t8_forest_get_user_data(forest);
  t8_messy_coarsen_t *coarsen = messy_data->coarsen;
  t8_latlon_data_chunk_t *data_chunk = messy_data->chunk;

  double max_error = 1e-6;

  /* calculate how many values one element has */
  int element_length = data_chunk->z_length * data_chunk->num_tracers;
  /* calculate start index for first element */
  int start = lelement_id * data_chunk->z_length * data_chunk->num_tracers;

  /* extract mass information, we expect the mass tracer as last tracer */
  int mass_index = data_chunk->num_tracers - 1;
  double mass[num_elements];
  double total_mass, interpolated, max;


  int z, z_offset, d, status;
  double values[num_elements];
  double errors[num_elements];

  /* loop over z-levels */
  for (z = 0; z < data_chunk->z_length; ++z) {
    /* calculate offset to z-layer in element */
    z_offset = z * data_chunk->num_tracers;

    /* calculate total mass of elements*/
    get_values(start + z_offset, num_elements, element_length, mass_index, mass, data_chunk->data);
    total_mass = sum(num_elements, mass, data_chunk->missing_value);

    /* loop over dimensions, but do not consider mass tracer */
    for(d = 0; d < data_chunk->num_tracers - 1; ++d) {

      /* extract values for the elements */
      get_values(start + z_offset, num_elements, element_length, d, values, data_chunk->data);

      /* calculate interpolated values */
      interpolated = (mult_sum(num_elements, values, mass, data_chunk->missing_value) / total_mass);

      /* calculate errors */
      calculate_error_ratios(num_elements, values, errors, interpolated, data_chunk->missing_value);

      /* get largest error ratio */
      max = get_max(errors, num_elements);

      /* if largest error ratio is larger than max ratio, we do not coarsen */
      if(max > 0.10) {
        //t8_debugf("error to large for z: %d, d: %d, error: %.12f \n", z, d, max);
        return 0;          
      }
    }
  }

  return -1;
}

/**
 * Callback function determening weather 4 cells can be combined.
 * 
 * 
 */
int
t8_messy_coarsen_callback (t8_forest_t forest,
                          t8_forest_t forest_from,
                          int which_tree,
                          int lelement_id,
                          t8_eclass_scheme_c * ts,
                          int num_elements, t8_element_t * elements[])
{
  int ret = 0;

  /* since we don't want to refine, 
     we can stop if we only have one element */
  if (num_elements == 1) {
    return ret;
  }

  t8_messy_data_t *messy_data = (t8_messy_data_t*) t8_forest_get_user_data(forest);
  t8_messy_coarsen_t *coarsen = messy_data->coarsen;
  t8_latlon_data_chunk_t *data_chunk = messy_data->chunk;

  t8_messy_custom_func_t* func_data = NULL;

  /* calculate how many values one element has */
  int element_length = data_chunk->z_length * data_chunk->num_tracers;
  /* calculate offset for z_layer */
  int z_offset = coarsen->z_layer * data_chunk->num_tracers;
  /* calculate start index for first element */
  int start = lelement_id * data_chunk->z_length * data_chunk->num_tracers;
  /* get tracer index */
  int tracer = t8_latlon_get_tracer_idx(data_chunk, coarsen->tracer, false);

  
  double values[num_elements];

  /* if we get an negative z-layer we min / max / mean over all layers first
      -1 = mean
      -2 = max
      -3 = min
   */
  if(coarsen->z_layer < 0) {
    //+ z_offset
    double temps[data_chunk->z_length];
    
    for (int e = 0; e < num_elements; ++e) {
      get_values(start, data_chunk->z_length, data_chunk->num_tracers, tracer, temps, data_chunk->data);
      switch(coarsen->z_layer) {
        case -1:
          values[e] = get_mean(temps, data_chunk->z_length);
          break;
        case -2:
          values[e] = get_max(temps, data_chunk->z_length);
          break;
        case -3:
          values[e] = get_min(temps, data_chunk->z_length);
          break;
      }
    }
  } else {
    /* otherwise grab the values for given z-layer */
    get_values(start + z_offset, num_elements, element_length, tracer, values, data_chunk->data);
  }

  double value;
  switch(coarsen->method) {
    case T8_MESSY_COARSEN_FUNCTION:
      /* TODO: implement  custom coarsening functions */
      func_data = t8_messy_new_custom_func(num_elements);
      func_data->z_layer = coarsen->z_layer;
      strcpy(func_data->tracer, coarsen->tracer);

      /* set lat, lon, x, y,  */

      get_values(start, num_elements, element_length, tracer, func_data->values, data_chunk->data);
      ret = (coarsen->func)(func_data);
      t8_messy_destroy_custom_func(func_data);
      func_data = NULL;
      break;
    case T8_MESSY_COARSEN_AREA_INSIDE:
      /* TODO: area coarsening */
      break;
    case T8_MESSY_COARSEN_AREA_OUTSIDE:
      /* TODO: area coarsening */
      break;
    case T8_MESSY_COARSEN_THRESHOLD_MIN_LOWER:
      value = get_min(values, num_elements);
      ret = value < coarsen->threshold ? -1 : 0;
      break;
    case T8_MESSY_COARSEN_THRESHOLD_MIN_HIGHER:
      value = get_min(values, num_elements);
      ret = value > coarsen->threshold ? -1 : 0;
      break;
    case T8_MESSY_COARSEN_THRESHOLD_MAX_LOWER:
      value = get_max(values, num_elements);
      ret = value < coarsen->threshold ? -1 : 0;
      break;
    case T8_MESSY_COARSEN_THRESHOLD_MAX_HIGHER:
      value = get_max(values, num_elements);
      ret = value > coarsen->threshold ? -1 : 0;
      break;
    case T8_MESSY_COARSEN_THRESHOLD_MEAN_LOWER:
      value = get_mean(values, num_elements);
      ret = value < coarsen->threshold ? -1 : 0;
      break;
    case T8_MESSY_COARSEN_THRESHOLD_MEAN_HIGHER:
      value = get_mean(values, num_elements);
      ret = value > coarsen->threshold ? -1 : 0;
      break;
  }


  return ret;
}

static void
t8_messy_interpolate_callback2 (t8_forest_t forest_old,
                   t8_forest_t forest_new,
                   t8_locidx_t which_tree,
                   t8_eclass_scheme_c * ts,
                   int num_outgoing, /* previously number of cells, only interesting when 4 */
                   t8_locidx_t first_outgoing, /* index  of first cell in forest_old */
                   int num_incoming, /* number of cells to be.., should be 1 */
                   t8_locidx_t first_incoming) /* index of new cell in forest_new */
{

  t8_messy_data_t *messy_data = (t8_messy_data_t *) t8_forest_get_user_data(forest_new);
  t8_messy_interpolate_t *interpolation = messy_data->interpolation;
  t8_latlon_data_chunk_t *data_chunk = messy_data->chunk;

  int num_elements_new = t8_forest_get_num_element(forest_new);
  int num_tracers = data_chunk->num_tracers;
  int z_length = data_chunk->z_length;
  int element_length = num_tracers * z_length;
  int index_incoming, index_outgoing;

  index_incoming = first_incoming * element_length;
  index_outgoing = first_outgoing * element_length;

  if(num_outgoing > num_incoming) {
    
    /* when the number of previous elements (num_outgoing) is larger than the number of created cell from it (num_incoming)
     * we interpolate,
     */


    /* extract mass information */
    /* we expect the mass tracer is always the first tracer added */
    int mass_index = num_tracers-1;
    double mass[num_outgoing];
    double total_mass, interpolated;

    double max_local, global, max;
    double errors[num_outgoing];
    double local_errors[num_outgoing];

    
    int d, z, z_offset, start;
    double values[num_outgoing];

    for(z = 0; z < z_length; ++z) {
      /* calculate offset for z_layer */
      z_offset = z * num_tracers;
      /* calculate start index for first element */
      start = index_outgoing + z_offset;

      /* calculate total mass of elements*/
      get_values(start, num_outgoing, element_length, mass_index, mass, data_chunk->data);
      total_mass = sum(num_outgoing, mass, data_chunk->missing_value);

      /* set new mass */
      data_chunk->data_adapt[index_incoming + z_offset + mass_index] = total_mass;

      for(d = 0; d < num_tracers - 1; ++d) {
        /* extract values for the elements */
        get_values(start, num_outgoing, element_length, d, values, data_chunk->data);

        /* calculate interpolated values */
        interpolated = (mult_sum(num_outgoing, values, mass, data_chunk->missing_value) / total_mass);

        /* set interpolated value*/
        data_chunk->data_adapt[index_incoming + z_offset + d] = interpolated;

        /* set error values */

        /* calculate error ratios */
        calculate_error_ratios(num_outgoing, values, local_errors, interpolated, data_chunk->missing_value);

        /* find largest local error ratio over all z-layers*/
        max_local = messy_data->errors_adapt[first_incoming * (num_tracers - 1) + d];
        max_local = fmax(max_local, get_max(local_errors, num_outgoing));

        /* set local error */
        messy_data->errors_adapt[first_incoming * (num_tracers - 1) + d] = max_local;

        /* set global error */
        global = messy_data->errors_global[first_incoming * (num_tracers - 1) + d];
        messy_data->errors_adapt_global[first_incoming * (num_tracers - 1) + d] = global + max_local;

      }
      
    }
        
  } else {
    /* else just copy data over to new array */
    memcpy (data_chunk->data_adapt + index_incoming,
            data_chunk->data       + index_outgoing,
              element_length * sizeof (double));

    /* copy errors over  */
    memcpy (messy_data->errors_adapt + first_incoming * (num_tracers - 1),
            messy_data->errors       + first_outgoing * (num_tracers - 1),
              (num_tracers - 1) * sizeof (double));
    
    memcpy (messy_data->errors_adapt_global + first_incoming * (num_tracers - 1),
            messy_data->errors_global       + first_outgoing * (num_tracers - 1),
              (num_tracers - 1) * sizeof (double));
  }

}

static void
t8_messy_interpolate_callback (t8_forest_t forest_old,
                   t8_forest_t forest_new,
                   t8_locidx_t which_tree,
                   t8_eclass_scheme_c * ts,
                   int num_outgoing, /* previously number of cells, only interesting when 4 */
                   t8_locidx_t first_outgoing, /* index  of first cell in forest_old */
                   int num_incoming, /* number of cells to be.., should be 1 */
                   t8_locidx_t first_incoming) /* index of new cell in forest_new */
{

  t8_messy_data_t *messy_data = (t8_messy_data_t *) t8_forest_get_user_data(forest_new);
  t8_messy_interpolate_t *interpolation = messy_data->interpolation;
  t8_latlon_data_chunk_t *data_chunk = messy_data->chunk;

  int index_incoming, index_outgoing;

  int num_tracers = data_chunk->num_tracers;
  int z_length = data_chunk->z_length;
  int element_data_length = num_tracers * z_length;

  index_incoming = first_incoming * element_data_length;
  index_outgoing = first_outgoing * element_data_length;

  if(num_outgoing > num_incoming) {
    
    /* when the number of previous elements (num_outgoing) is larger than the number of created cell from it (num_incoming)
     * we interpolate,
     */
    t8_messy_custom_func_t* func_data = NULL;

    int d, z, z_offset, start;
    double values[num_outgoing];
    double value;
    
    switch(interpolation->method) {
      case T8_MESSY_INTERPOLATE_FUNCTION:
        /* TODO: implement custom interpolation */
        func_data = t8_messy_new_custom_func(num_outgoing);
        
        /* loop over elements and fill lat, lon, x, y in func data */

        for(z = 0; z < z_length; ++z) {
          func_data->z_layer = z;
          /* calculate offset for z_layer */
          z_offset = z * num_tracers;
          /* calculate start index for first element */
          start = index_outgoing + z_offset;
          for(d = 0; d < num_tracers; ++d) {
            strcpy(func_data->tracer, data_chunk->tracer_names + d * BUFSIZ);
            get_values(start, num_outgoing, element_data_length, d, func_data->values, data_chunk->data);
            value = (double)(interpolation->func)(func_data);
            data_chunk->data_adapt[index_incoming + z_offset + d] = value;
          }
        }
        t8_messy_destroy_custom_func(func_data);
        func_data = NULL;
        break;

      case T8_MESSY_INTERPOLATE_MIN:
        for(z = 0; z < z_length; ++z) {
          /* calculate offset for z_layer */
          z_offset = z * num_tracers;
          /* calculate start index for first element */
          start = index_outgoing + z_offset;
          for(d = 0; d < num_tracers; ++d) {
            get_values(start, num_outgoing, element_data_length, d, values, data_chunk->data);
            data_chunk->data_adapt[index_incoming + z_offset + d] = get_min(values, num_outgoing);
          }
        }
        break;

      case T8_MESSY_INTERPOLATE_MAX:
        for(z = 0; z < z_length; ++z) {
          /* calculate offset for z_layer */
          z_offset = z * num_tracers;
          /* calculate start index for first element */
          start = index_outgoing + z_offset;
          for(d = 0; d < num_tracers; ++d) {
            get_values(start, num_outgoing, element_data_length, d, values, data_chunk->data);
            data_chunk->data_adapt[index_incoming + z_offset + d] = get_max(values, num_outgoing);
          }
        }
        break;

      case T8_MESSY_INTERPOLATE_MEAN:
        for(z = 0; z < z_length; ++z) {
          /* calculate offset for z_layer */
          z_offset = z * num_tracers;
          /* calculate start index for first element */
          start = index_outgoing + z_offset;
          for(d = 0; d < num_tracers; ++d) {
            get_values(start, num_outgoing, element_data_length, d, values, data_chunk->data);
            data_chunk->data_adapt[index_incoming + z_offset + d] = get_mean(values, num_outgoing);
          }
        }
        break;
    }

  } else {
    /* else just copy data over to new array */
    memcpy (data_chunk->data_adapt + index_incoming,
            data_chunk->data       + index_outgoing,
              element_data_length * sizeof (double));
  }

}

t8_messy_coarsen_t* t8_messy_new_coarsen_config(
  const char* method,
  char* tracer,
  int z_layer,
  double threshold,
  int (*func)(t8_messy_custom_func_t *)
 ) {
  t8_messy_coarsen_t* config = T8_ALLOC(t8_messy_coarsen_t, 1);
  
  config->tracer = tracer;
  config->z_layer = z_layer;

  config->threshold = threshold;
  
  if(strcmp(method, "mean_lower") == 0) {
    config->method = T8_MESSY_COARSEN_THRESHOLD_MEAN_LOWER;
  } else if(strcmp(method, "mean_higher") == 0) {
    config->method = T8_MESSY_COARSEN_THRESHOLD_MEAN_HIGHER;
  } else if(strcmp(method, "min_lower") == 0) {
    config->method = T8_MESSY_COARSEN_THRESHOLD_MIN_LOWER;
  } else if(strcmp(method, "min_higher") == 0) {
    config->method = T8_MESSY_COARSEN_THRESHOLD_MIN_HIGHER;
  } else if(strcmp(method, "max_lower") == 0) {
    config->method = T8_MESSY_COARSEN_THRESHOLD_MAX_LOWER;
  } else if(strcmp(method, "max_higher") == 0) {
    config->method = T8_MESSY_COARSEN_THRESHOLD_MAX_HIGHER;
  } else if(strcmp(method, "custom") == 0) {
    config->method = T8_MESSY_COARSEN_FUNCTION;
  } else {
    config->method = T8_MESSY_COARSEN_THRESHOLD_MEAN_LOWER;
  }
  
  config->func = func;

  return config;
}


t8_messy_interpolate_t* t8_messy_new_interpolate_config(
  const char* method,
  double (*func)(t8_messy_custom_func_t *)
) {
  t8_messy_interpolate_t *config = T8_ALLOC(t8_messy_interpolate_t, 1);

  if(strcmp(method, "mean") == 0) {
    config->method = T8_MESSY_INTERPOLATE_MEAN;
  } else if(strcmp(method, "min") == 0) {
    config->method = T8_MESSY_INTERPOLATE_MIN;
  } else if(strcmp(method, "max") == 0) {
    config->method = T8_MESSY_INTERPOLATE_MAX;
  } else if(strcmp(method, "custom") == 0) {
    config->method = T8_MESSY_INTERPOLATE_FUNCTION;
  } else {
    config->method = T8_MESSY_INTERPOLATE_MEAN;
  }

  config->func = func;

  return config;
}


t8_messy_data_t* t8_messy_initialize(
  const char* description,
  const char* axis,
  int* shape,
  int x_start, 
  int y_start,
  int num_tracers,
  double missing_value,
  t8_messy_coarsen_t *coarsen,
  t8_messy_interpolate_t *interpolation
  ) {

  #ifdef T8_ENABLE_DEBUG
    t8_global_productionf("Initializing MESSy coupler\n");
  #endif

  t8_debugf("missing_value %.14f", missing_value);

  /* determine axes */
  int x, y, z;
  int x_length, y_length, z_length;
  int x_axis, y_axis, z_axis;

  const char *c = strchr(axis, 'X');
  x = c - axis;

  const char *d = strchr(axis, 'Y');
  y = d - axis;

  const char *e = strchr(axis, 'Z');
  z = e - axis;

  /* TODO safeguard if one axis is not found */

  #ifdef T8_ENABLE_DEBUG
    t8_global_productionf("x: %d, y: %d, z: %d\n", x, y, z);
  #endif

  /* assign correct axis axis */
  x_axis =  x >= 0 && x <= 3 ? x : fmax(y, z) + 1;
  y_axis =  y >= 0 && y <= 3 ? y : fmax(x, z) + 1;
  z_axis =  z >= 0 && z <= 3 ? z : fmax(x, y) + 1;

  /* assign correct axis length */
  x_length = x >= 0 && x <= 3 ? shape[x] : 1;
  y_length = y >= 0 && y <= 3 ? shape[y] : 1;
  z_length = z >= 0 && z <= 3 ? shape[z] : 1;
  
  #ifdef T8_ENABLE_DEBUG
    t8_global_productionf("xaxis: %d, yaxis: %d, zaxis: %d\n", x_axis, y_axis, z_axis);
    t8_global_productionf("x_length: %d, y_length: %d, z_length: %d\n", x_length, y_length, z_length);
  #endif

  /* create forest for smallest mesh which completely contains given messy mesh */
  t8_forest_t forest = t8_latlon_refine(x_length, y_length, T8_LATLON_COARSEN, 0);
  t8_latlon_adapt_data_t *adapt_data =
    (t8_latlon_adapt_data_t *) t8_forest_get_user_data (forest);
  
  int* lshape = T8_ALLOC_ZERO(int, 3);
  memcpy(lshape, shape, sizeof(int) * 3);

  /* create data chunk */
  t8_latlon_data_chunk_t *chunk = t8_latlon_new_chunk(
    description,
    x_start, y_start,
    x_length, y_length, z_length,
    lshape,
    num_tracers, x_axis, y_axis, z_axis, 
    adapt_data->max_level,
    missing_value,
    T8_LATLON_DATA_MESSY);

  t8_messy_data_t* messy_data = T8_ALLOC(t8_messy_data_t, 1);
  messy_data->chunk = chunk;
  messy_data->forest = forest;
  messy_data->coarsen = coarsen;
  messy_data->interpolation = interpolation;
  messy_data->counter = 0;
  messy_data->errors = NULL;
  messy_data->errors_adapt = NULL;
  messy_data->errors_global = NULL;
  messy_data->errors_adapt_global = NULL;

  #ifdef T8_ENABLE_DEBUG
    t8_global_productionf("MESSy coupler initialized\n");
  #endif

  return messy_data;
}

void t8_messy_reset(t8_messy_data_t* messy_data) {
  T8_FREE(messy_data->errors);
  T8_FREE(messy_data->errors_adapt);
  T8_FREE(messy_data->errors_global);
  T8_FREE(messy_data->errors_adapt_global);
  
  messy_data->errors = NULL;
  messy_data->errors_adapt = NULL;
  messy_data->errors_global = NULL;
  messy_data->errors_adapt_global = NULL;

  t8_latlon_data_chunk_t *chunk = messy_data->chunk;
  if(chunk->numbering == T8_LATLON_DATA_MORTON) {
    /* reset data chunk if we already applied morton order */
    T8_FREE(chunk->data);
    T8_FREE(chunk->data_ids);
    T8_FREE(chunk->data_adapt);
    T8_FREE(chunk->data_ids_adapt);
    chunk->data = T8_ALLOC_ZERO(double, chunk->x_length * chunk->y_length * chunk->z_length * chunk->num_tracers );
    chunk->data_ids = T8_ALLOC_ZERO(t8_linearidx_t, chunk->x_length * chunk->y_length);
    chunk->numbering = T8_LATLON_DATA_MESSY;
  }
  t8_debugf("messy data resetted \n");
}

int t8_messy_get_max_number_elements(t8_messy_data_t* messy_data) {
  return t8_forest_get_num_element(messy_data->forest);
}

void t8_messy_add_dimension(t8_messy_data_t *messy_data, char* dimension_name, double ****data) {
  t8_latlon_set_dimension(messy_data->chunk, dimension_name, data);
}

// Stores the trimmed input string into the given output buffer, which must be
// large enough to store the result.  If it is too small, the output is
// truncated.
size_t trimwhitespace(char *out, size_t len, const char *str)
{
  if(len == 0)
    return 0;

  const char *end;
  size_t out_size;

  // Trim leading space
  while(isspace((unsigned char)*str)) str++;

  if(*str == 0)  // All spaces?
  {
    *out = 0;
    return 1;
  }

  // Trim trailing space
  end = str + strlen(str) - 1;
  while(end > str && isspace((unsigned char)*end)) end--;
  end++;

  // Set output size to minimum of trimmed string length and buffer size minus 1
  out_size = (end - str) < len-1 ? (end - str) : len-1;

  // Copy trimmed string and add null terminator
  memcpy(out, str, out_size);
  out[out_size] = 0;

  return out_size;
}

void t8_messy_set_tracer_values(t8_messy_data_t *messy_data, char* tracer_name, double *data) {
  t8_latlon_data_chunk_t *data_chunk = messy_data->chunk;
  char* name = T8_ALLOC(char, BUFSIZ);
  
  trimwhitespace(name, BUFSIZ, tracer_name);
  
  // get index for tracer and and matching tracer error
  int tracer_index = t8_latlon_get_tracer_idx(data_chunk, name, data_chunk->tracer_names_size < data_chunk->num_tracers);
  
  T8_ASSERT(tracer_index > -1);

  t8_debugf("set values for tracer %s at index %d\n", name, tracer_index);

  int size = data_chunk->x_length * data_chunk->y_length * data_chunk->z_length;
  int len = data_chunk->shape[0] * data_chunk->shape[1];
  
  int i, l, x, y, z, data_index;
  int *idx = T8_ALLOC_ZERO(int, 3);

  // t8_debugf("[%d, %d, %d]\n", data_chunk->shape[0], data_chunk->shape[1], data_chunk->shape[2]);

  for(i=0; i<size; i++) {
    idx[0] = i / len;
    l      = i % len;
    idx[1] = l / data_chunk->shape[0];
    idx[2] = l % data_chunk->shape[0];
  
    /* set correct coordinates */
    x = idx[2 - data_chunk->x_axis];
    y = (data_chunk->y_length - 1) - idx[2 - data_chunk->y_axis];
    z = idx[2 - data_chunk->z_axis];

    /* calculate index in data array */
    data_index = ((y * data_chunk->z_length * data_chunk->x_length + x * data_chunk->z_length + z) * data_chunk->num_tracers) + tracer_index;

    /* copy data */
    // t8_debugf("(%d)[%d, %d, %d](%d): %.16f\n", i, x, y, z, data_index, data[i]);
    memcpy((data_chunk->data) + data_index, data + i, sizeof(double));
  }

  T8_FREE(name);
  T8_FREE(idx);
}

void t8_messy_apply_sfc(t8_messy_data_t *messy_data) {
  t8_latlon_data_apply_morton_order(&(messy_data->forest), messy_data->chunk);
}

void t8_messy_coarsen(t8_messy_data_t *messy_data) {
  char vtu_prefix[BUFSIZ];

  t8_global_productionf("MESSy coarsen grid \n");

  /* check if coarsening and interpolation configuration is set */
  T8_ASSERT(messy_data->coarsen != NULL);
  T8_ASSERT(messy_data->interpolation != NULL);

  /* check if custom coarsen function is supplied */
  if(messy_data->coarsen->method == T8_MESSY_COARSEN_FUNCTION) {
    /* if coarsening method is custom function, check one is given */
    T8_ASSERT(messy_data->coarsen->func != NULL);
  }

  /* check if custom interpolation function is supplied */
  if(messy_data->interpolation->method == T8_MESSY_INTERPOLATE_FUNCTION) {
    /* if interpolation method is custom function, check one is given */
    T8_ASSERT(messy_data->interpolation->func != NULL);
  }

  t8_latlon_data_chunk_t *data_chunk = messy_data->chunk;


  t8_forest_t forest;
  t8_forest_t forest_adapt;

  forest = messy_data->forest;
  t8_forest_ref(forest);
  // forest ref = 2
  int last_num_elements = 0, num_elements = 0, r;
  
  num_elements = t8_forest_get_num_element(forest);

  messy_data->errors = T8_ALLOC_ZERO(double, num_elements * (data_chunk->num_tracers - 1));
  messy_data->errors_global = T8_ALLOC_ZERO(double, num_elements * (data_chunk->num_tracers - 1));

  #ifdef T8_ENABLE_DEBUG
    /* In debugging mode write the forest */
    snprintf (vtu_prefix, BUFSIZ, "t8_messy_grid_step_%d", messy_data->counter);
    t8_messy_write_forest(forest, vtu_prefix, messy_data);
  #endif

  for(r=0; r < 10; ++r) {

    t8_forest_ref(forest);
    // forest ref = 2
    // adapt ref = 2
    forest_adapt = t8_forest_new_adapt(forest, t8_messy_coarsen_by_error_tol_callback, 0, 0, messy_data);
    // forest ref = 1
    // adapt ref = 1

    num_elements = t8_forest_get_num_element(forest_adapt);

    /* check if anything changed */
    if(num_elements == last_num_elements) {
      /* adapt step did not change anything so we can already stop loop*/
      break;
    }
    last_num_elements = num_elements;

    data_chunk->data_ids_adapt = T8_ALLOC_ZERO(t8_linearidx_t, num_elements);
    data_chunk->data_adapt = T8_ALLOC_ZERO(double, num_elements * data_chunk->z_length * data_chunk->num_tracers);
    messy_data->errors_adapt = T8_ALLOC_ZERO(double, num_elements * (data_chunk->num_tracers - 1));
    messy_data->errors_adapt_global = T8_ALLOC_ZERO(double, num_elements * (data_chunk->num_tracers - 1));

    t8_forest_iterate_replace(forest_adapt, forest, t8_messy_interpolate_callback2);

    T8_FREE(data_chunk->data_ids);
    T8_FREE(data_chunk->data);
    T8_FREE(messy_data->errors);
    T8_FREE(messy_data->errors_global);

    data_chunk->data_ids = data_chunk->data_ids_adapt;
    data_chunk->data = data_chunk->data_adapt;
    messy_data->errors = messy_data->errors_adapt;
    messy_data->errors_global = messy_data->errors_adapt_global;

    data_chunk->data_ids_adapt = NULL;
    data_chunk->data_adapt = NULL;
    messy_data->errors_adapt = NULL;
    messy_data->errors_adapt_global = NULL;

    t8_forest_unref(&forest);
    // forest ref = 0
    // adapt ref = 1
    forest = forest_adapt;
    // forest ref = 1
    // adapt ref = 1

    #ifdef T8_ENABLE_DEBUG
      /* In debugging mode write the forest */
      snprintf (vtu_prefix, BUFSIZ, "t8_messy_grid_interpolated_step_%d_%d", messy_data->counter, r);
      t8_messy_write_forest(forest_adapt, vtu_prefix, messy_data);
    #endif

  }
  
  t8_forest_unref(&forest_adapt);
  t8_forest_unref(&forest);

  messy_data->num_elements = last_num_elements;
  messy_data->counter = messy_data->counter + 1;

  t8_global_productionf("MESSy grid coarsening done (%d rounds) \n", r);

}


void t8_messy_write_tracer_values(t8_messy_data_t* messy_data, const char* tracer_name, double* data) {
  t8_latlon_data_chunk_t *data_chunk = messy_data->chunk;
  
  char* name = T8_ALLOC(char, BUFSIZ);
  trimwhitespace(name, BUFSIZ, tracer_name);

  int num_elements = messy_data->num_elements;
  int tracer_index = t8_latlon_get_tracer_idx(data_chunk, name, false);
  int num_data = data_chunk->z_length * data_chunk->num_tracers;

  int element, z, offset, index = 0;
  for(z=0; z<data_chunk->z_length; ++z) {
    for(element=0; element<num_elements; ++element) {
      offset = element * num_data + z * data_chunk->num_tracers + tracer_index;

      data[index++] = data_chunk->data[offset];
    }
  }

}

void t8_messy_destroy(t8_messy_data_t* messy_data) {
  t8_latlon_chunk_destroy(&(messy_data->chunk));
  t8_forest_unref(&(messy_data->forest));
  T8_FREE(messy_data->coarsen);
  T8_FREE(messy_data->interpolation);
  T8_FREE(messy_data->errors);
  T8_FREE(messy_data->errors_adapt);
  T8_FREE(messy_data->errors_global);
  T8_FREE(messy_data->errors_adapt_global);
  T8_FREE(messy_data);
}

void t8_messy_write_forest(t8_forest_t forest, const char* prefix, t8_messy_data_t* messy_data) {

  t8_latlon_data_chunk_t *data_chunk = messy_data->chunk;
  int num_elements = t8_forest_get_num_element(forest);
  int num_data = data_chunk->num_tracers * data_chunk->z_length;
  int num_data_out = num_data + (data_chunk->num_tracers - 1) * 2;
 
 
  t8_debugf("dims %d, z_len %d, num elements %d, num data %d\n", data_chunk->num_tracers, data_chunk->z_length, num_elements, num_data);
  /* TODO: Do not use static array with variable as length */
  t8_vtk_data_field_t vtk_data[num_data_out];
  double *dim_data_array[num_data_out];
  
  int z, d, e, offset, j;
  for(z = 0; z < data_chunk->z_length; ++z) { 
    for(d = 0; d < data_chunk->num_tracers; ++d) {
      offset = z * data_chunk->num_tracers + d;
      dim_data_array[offset] = T8_ALLOC_ZERO (double, num_elements);
      for(e = 0; e < num_elements; ++e) {
        dim_data_array[offset][e] = data_chunk->data[e * num_data + z * data_chunk->num_tracers + d];
      }
      snprintf (vtk_data[offset].description, BUFSIZ, "z%d_%s", z, data_chunk->tracer_names + d * BUFSIZ);
      
      vtk_data[offset].type = T8_VTK_SCALAR;
      vtk_data[offset].data = (double*) dim_data_array[offset];
    }
  }

  /* add local error layers */
  for(offset+=1, j=0; j < (data_chunk->num_tracers-1); ++offset, ++j) {
    dim_data_array[offset] = T8_ALLOC_ZERO (double, num_elements);
    for(e = 0; e < num_elements; ++e) {
      dim_data_array[offset][e] = messy_data->errors[e * (data_chunk->num_tracers-1) + j];
    }
    snprintf (vtk_data[offset].description, BUFSIZ, "local_error_%s", data_chunk->tracer_names + j * BUFSIZ);

    vtk_data[offset].type = T8_VTK_SCALAR;
    vtk_data[offset].data = (double*) dim_data_array[offset];
  }

  /* add global error layers */
  for(j=0; j < (data_chunk->num_tracers-1); ++offset, ++j) {
    dim_data_array[offset] = T8_ALLOC_ZERO (double, num_elements);
    for(e = 0; e < num_elements; ++e) {
      dim_data_array[offset][e] = messy_data->errors_global[e * (data_chunk->num_tracers-1) + j];
    }
    snprintf (vtk_data[offset].description, BUFSIZ, "global_error_%s", data_chunk->tracer_names + j * BUFSIZ);

    vtk_data[offset].type = T8_VTK_SCALAR;
    vtk_data[offset].data = (double*) dim_data_array[offset];
  }

  t8_forest_vtk_write_file (forest, prefix, 1, 1, 1, 1, 0, num_data_out, vtk_data);

  for(offset = 0; offset < num_data_out; ++offset) {
    T8_FREE(dim_data_array[offset]);
  }
  
}