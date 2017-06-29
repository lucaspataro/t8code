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

#include "t8_dprism_bits.h"
#include "t8_dline_bits.h"
#include "t8_dtri_bits.h"
#include "sc_functions.h"
#include "p4est.h"

int
t8_dprism_get_level (const t8_dprism_t * p)
{
  T8_ASSERT (p->line.level == p->tri.level);
  return p->line.level;
}

void
t8_dprism_copy (const t8_dprism_t * p, t8_dprism_t * dest)
{
  T8_ASSERT (p->line.level == p->tri.level);
  memcpy (dest, p, sizeof (t8_dprism_t));
  T8_ASSERT (dest->line.level == dest->tri.level);
}

int
t8_dprism_compare (const t8_dprism_t * p1, const t8_dprism_t * p2)
{
  int                 maxlvl;
  u_int64_t           id1, id2;
  T8_ASSERT (p1->line.level == p1->tri.level);
  T8_ASSERT (p2->line.level == p2->tri.level);

  maxlvl = SC_MAX (p1->line.level, p2->line.level);
  /* Compute the linear ids of the elements */
  id1 = t8_dprism_linear_id (p1, maxlvl);
  id2 = t8_dprism_linear_id (p2, maxlvl);
  if (id1 == id2) {
    /* The linear ids are the same, the prism with the smaller level
     * is considered smaller */
    return p1->line.level - p2->line.level;
  }
  /* return negativ if id1 < id2, zero if id1 = id2, positive if id1 >
     id2 */
  return id1 < id2 ? -1 : id1 != id2;
}

void
t8_dprism_init_linear_id (t8_dprism_t * p, int level, uint64_t id)
{
  uint64_t            tri_id = 0;
  uint64_t            line_id = 0;
  int                 i;
  int                 triangles_of_size_i = 1;

  T8_ASSERT (0 <= level && level <= T8_DPRISM_MAXLEVEL);
  T8_ASSERT (id < sc_intpow (T8_DPRISM_CHILDREN, level));

  for (i = 0; i <= level; i++) {
    /*Get the number of the i-th prism and get the related triangle number
     * then multiplicate it by the number of triangles of level size.*/
    tri_id +=
      ((id % T8_DPRISM_CHILDREN) % T8_DTRI_CHILDREN) * triangles_of_size_i;

    /*If id % 8 is larger than 3, the prism is in the upper part of the
     * parent prism. => line_id + 2^i*/
    line_id +=
      (id % T8_DPRISM_CHILDREN) / T8_DTRI_CHILDREN *
      sc_intpow (T8_DLINE_CHILDREN, i);

    /*Each Prism divides into 8 children */
    id /= T8_DPRISM_CHILDREN;
    /*Each triangle divides into 4 children */
    triangles_of_size_i *= T8_DTRI_CHILDREN;
  }
  t8_dtri_init_linear_id (&p->tri, tri_id, level);
  t8_dline_init_linear_id (&p->line, level, line_id);

  T8_ASSERT (p->line.level == p->tri.level);
}

void
t8_dprism_parent (const t8_dprism_t * p, t8_dprism_t * parent)
{
  T8_ASSERT (p->line.level > 0);
  T8_ASSERT (p->line.level == p->tri.level);

  t8_dtri_parent (&p->tri, &parent->tri);
  t8_dline_parent (&p->line, &parent->line);

  T8_ASSERT (parent->line.level == parent->tri.level);
}

int
t8_dprism_child_id (const t8_dprism_t * p)
{
  int                 tri_child_id = t8_dtri_child_id (&p->tri);
  int                 line_child_id = t8_dline_child_id (&p->line);
  T8_ASSERT (p->line.level == p->tri.level);
  /*Prism in lower plane has the same id as the triangle, in the upper plane
   * it's a shift by the number of children a triangle has*/
  return tri_child_id + T8_DTRI_CHILDREN * line_child_id;
}

int
t8_dprism_is_familypv (t8_dprism_t ** fam)
{
  int                 i, j;
  t8_dtri_t         **tri_fam = T8_ALLOC (t8_dtri_t *, T8_DTRI_CHILDREN);
  t8_dline_t        **line_fam = T8_ALLOC (t8_dline_t *, T8_DLINE_CHILDREN);
  int                 is_family = 1;

  for (i = 0; i < T8_DLINE_CHILDREN; i++) {
    for (j = 0; j < T8_DTRI_CHILDREN; j++) {
      tri_fam[j] = &fam[j + i * T8_DTRI_CHILDREN]->tri;
    }
    is_family = is_family
      && t8_dtri_is_familypv ((const t8_dtri_t **) tri_fam);
  }
  for (i = 0; i < T8_DTRI_CHILDREN; i++) {
    for (j = 0; j < T8_DLINE_CHILDREN; j++) {
      line_fam[j] = &fam[j * T8_DTRI_CHILDREN + i]->line;
    }
    /*Proof for line_family and equality of triangles in both planes */
    is_family = is_family
      && t8_dline_is_familypv ((const t8_dline_t **) line_fam)
      && (fam[i]->tri.level == fam[i + T8_DTRI_CHILDREN]->tri.level)
      && (fam[i]->tri.type == fam[i + T8_DTRI_CHILDREN]->tri.type)
      && (fam[i]->tri.x == fam[i + T8_DTRI_CHILDREN]->tri.x)
      && (fam[i]->tri.y == fam[i + T8_DTRI_CHILDREN]->tri.y);
  }
  for (i = 0; i < T8_DPRISM_CHILDREN; i++) {
    is_family = is_family && (fam[i]->line.level == fam[i]->tri.level);
  }
  T8_FREE (tri_fam);
  T8_FREE (line_fam);
  return is_family;
}

void
t8_dprism_boundary_face (const t8_dprism_t * p, int face,
                         t8_element_t * boundary)
{
  /*TODO: Cases überlegen, Koordinatensystem hängt von face ab */
  T8_ASSERT (0 <= face && face < T8_DPRISM_FACES);
  /* if (face < 3) {
     p4est_quadrant_t   *l = (p4est_quadrant_t *) boundary;
     l->x = p->tri.x;
     l->y = p->line.x;
     l->level = p->tri.level;
     } */
  p4est_quadrant_t   *q = (p4est_quadrant_t *) boundary;
  if (face >= 3) {
    t8_dtri_t          *l = (t8_dtri_t *) boundary;
    l->level = p->tri.level * (1 << (T8_DTRI_MAXLEVEL - T8_DPRISM_MAXLEVEL));
    l->type = p->tri.type;
    l->x = p->tri.x * (1 << (T8_DTRI_MAXLEVEL - T8_DPRISM_MAXLEVEL));
    l->y = p->tri.y * (1 << (T8_DTRI_MAXLEVEL - T8_DPRISM_MAXLEVEL));
    return;
  }
  switch (face) {
  case 0:
    q->x = p->tri.y * (1 << (P4EST_MAXLEVEL - T8_DPRISM_MAXLEVEL));
    q->y = p->line.x * (1 << (P4EST_MAXLEVEL - T8_DPRISM_MAXLEVEL));
    q->level = p->tri.level;
    break;
  case 1:
    q->x = p->tri.x * (1 << (P4EST_MAXLEVEL - T8_DPRISM_MAXLEVEL));
    q->y = p->line.x * (1 << (P4EST_MAXLEVEL - T8_DPRISM_MAXLEVEL));
    q->level = p->tri.level;
    break;
  case 2:
    q->x = p->tri.x * (1 << (P4EST_MAXLEVEL - T8_DPRISM_MAXLEVEL));
    q->y = p->line.x * (1 << (P4EST_MAXLEVEL - T8_DPRISM_MAXLEVEL));
    q->level = p->tri.level;
    break;
  default:
    SC_ABORT_NOT_REACHED ();
  }
}

int
t8_dprism_is_root_boundary (const t8_dprism_t * p, int face)
{
  T8_ASSERT (0 <= face && face < T8_DPRISM_FACES);
  /*face is not the bottom or top face of a prism */
  if (face < 3) {
    return t8_dtri_is_root_boundary (&p->tri, face);
  }
  else {
    return t8_dline_is_inside_root (&p->line);
  }
}

int
t8_dprism_is_inside_root (t8_dprism_t * p)
{
  return t8_dtri_is_inside_root (&p->tri) &&
    t8_dline_is_inside_root (&p->line);
}

void
t8_dprism_child (const t8_dprism_t * p, int childid, t8_dprism_t * child)
{
  T8_ASSERT (0 <= childid && childid < T8_DPRISM_CHILDREN);
  T8_ASSERT (p->line.level == p->tri.level);

  t8_dtri_child (&p->tri, childid % T8_DTRI_CHILDREN, &child->tri);
  t8_dline_child (&p->line, childid / T8_DTRI_CHILDREN, &child->line);

  T8_ASSERT (child->line.level == child->tri.level);
}

int
t8_dprism_num_face_children (const t8_dprism_t * p, int face)
{
  T8_ASSERT (0 <= face && face < T8_DPRISM_FACES);
  /*Bottom and top have T8_DTRI_CHILDREN, the other three faces depend on
     the children the triangle face has */
  return (face >= 3 ? T8_DTRI_CHILDREN : T8_DTRI_FACE_CHILDREN *
          T8_DLINE_CHILDREN);
}

void
t8_dprism_face_neighbour (const t8_dprism_t * p, int face,
                          t8_dprism_t * neigh)
{
  T8_ASSERT (0 <= face && face < T8_DPRISM_FACES);
  if (face < 3) {
    t8_dline_copy (&p->line, &neigh->line);
    t8_dtri_face_neighbour (&p->tri, face, &neigh->tri);
  }
  else if (face == 3) {
    t8_dtri_copy (&p->tri, &neigh->tri);
    t8_dline_face_neighbour (&p->line, 0, &neigh->line);
  }
  else {
    t8_dtri_copy (&p->tri, &neigh->tri);
    t8_dline_face_neighbour (&p->line, 1, &neigh->line);
  }
}

void
t8_dprism_childrenpv (const t8_dprism_t * p, int length, t8_dprism_t * c[])
{
  int                 i;
  T8_ASSERT (length == T8_DPRISM_CHILDREN);
  T8_ASSERT (p->line.level < T8_DPRISM_MAXLEVEL &&
             p->tri.level == p->line.level);
  for (i = 7; i >= 0; i--) {
    t8_dprism_child (p, i, c[i]);
  }
}

void
t8_dprism_children_at_face (const t8_dprism_t * p,
                            int face, t8_dprism_t ** children,
                            int num_children)
{
  T8_ASSERT (num_children == t8_dprism_num_face_children (p, face));
  T8_ASSERT (0 <= face && face < T8_DPRISM_FACES);
  switch (face) {
  case 0:
    t8_dprism_child (p, 1, children[0]);
    t8_dprism_child (p, 3, children[1]);
    t8_dprism_child (p, 5, children[2]);
    t8_dprism_child (p, 7, children[3]);
    break;
  case 1:
    t8_dprism_child (p, 0, children[0]);
    t8_dprism_child (p, 3, children[1]);
    t8_dprism_child (p, 4, children[2]);
    t8_dprism_child (p, 7, children[3]);
    break;
  case 2:
    t8_dprism_child (p, 0, children[0]);
    t8_dprism_child (p, 1, children[1]);
    t8_dprism_child (p, 4, children[2]);
    t8_dprism_child (p, 5, children[3]);
    break;
  case 3:
    t8_dprism_child (p, 0, children[0]);
    t8_dprism_child (p, 1, children[1]);
    t8_dprism_child (p, 2, children[2]);
    t8_dprism_child (p, 3, children[3]);
    break;
  case 4:
    t8_dprism_child (p, 4, children[0]);
    t8_dprism_child (p, 5, children[1]);
    t8_dprism_child (p, 6, children[2]);
    t8_dprism_child (p, 7, children[3]);
    break;
  default:
    SC_ABORT_NOT_REACHED ();
  }
}

int
t8_dprism_face_child_face (const t8_dprism_t * elem, int face, int face_child)
{
  T8_ASSERT (0 <= face && face < T8_DPRISM_FACES);
  /* For prisms the face number of the children is the same as the one
   * of the parent */
  return face;
}

int
t8_dprism_tree_face (const t8_dprism_t * p, int face)
{
  T8_ASSERT (0 <= face && face < T8_DPRISM_FACES);
  /*For prisms, the face number coincides with the number of the root
     tree face */
  return face;
}

void
t8_dprism_extrude_face (const t8_element_t * face, t8_element_t * elem,
                        int root_face)
{
  t8_dprism_t        *p = (t8_dprism_t *) elem;
  const t8_dtri_t    *t = (const t8_dtri_t *) face;
  const p4est_quadrant_t *q = (const p4est_quadrant_t *) face;
  /*All boundary prisms have triangletype 0 */
  p->tri.type = 0;

  T8_ASSERT (0 <= root_face && root_face < T8_DPRISM_FACES);

  switch (root_face) {
  case 0:
    p->line.level = q->level;
    p->tri.level = q->level;
    p->tri.x = (1 << T8_DPRISM_MAXLEVEL) - T8_DPRISM_LEN (p->line.level);
    p->tri.y = q->x * T8_DPRISM_ROOT_BY_QUAD_ROOT;
    p->line.x = q->y * T8_DPRISM_ROOT_BY_QUAD_ROOT;
    break;
  case 1:
    p->line.level = q->level;
    p->tri.level = q->level;
    p->tri.x = q->x * T8_DPRISM_ROOT_BY_QUAD_ROOT;
    p->tri.y = q->x * T8_DPRISM_ROOT_BY_QUAD_ROOT;
    p->line.x = q->y * T8_DPRISM_ROOT_BY_QUAD_ROOT;
    break;
  case 2:
    p->line.level = q->level;
    p->tri.level = q->level;
    p->tri.x = q->x * T8_DPRISM_ROOT_BY_QUAD_ROOT;
    p->tri.y = 0;
    p->line.x = q->y * T8_DPRISM_ROOT_BY_QUAD_ROOT;
    break;
  case 3:
    p->line.level = t->level;
    p->tri.level = t->level;
    p->tri.x = t->x * T8_DPRISM_ROOT_BY_DTRI_ROOT;
    p->tri.y = t->y * T8_DPRISM_ROOT_BY_DTRI_ROOT;
    p->line.x = 0;
    break;
  case 4:
    p->line.level = t->level;
    p->tri.level = t->level;
    p->tri.x = t->x * T8_DPRISM_ROOT_BY_DTRI_ROOT;
    p->tri.y = t->y * T8_DPRISM_ROOT_BY_DTRI_ROOT;
    p->line.x = (1 << T8_DPRISM_MAXLEVEL) - T8_DPRISM_LEN (t->level);
    break;
  default:
    SC_ABORT_NOT_REACHED ();
  }
}

void
t8_dprism_successor (const t8_dprism_t * p, t8_dprism_t * succ, int level)
{
  int                 prism_child_id;
  t8_dprism_copy (p, succ);
  /*update the level */
  succ->line.level = level;
  succ->tri.level = level;
  prism_child_id = t8_dprism_child_id (succ);

  T8_ASSERT (1 <= level && level <= T8_DPRISM_MAXLEVEL);
  T8_ASSERT (p->line.level == p->tri.level);

  /*The next prism is the child with local ID 0 of the next parent prism */
  if (prism_child_id == T8_DPRISM_CHILDREN - 1) {
    t8_dprism_successor (p, succ, level - 1);
    /*Zero out the bits of higher level, caused by recursion */
#if 1
    succ->tri.x =
      (succ->tri.
       x >> (T8_DPRISM_MAXLEVEL - level + 1)) << (T8_DPRISM_MAXLEVEL - level +
                                                  1);
    succ->tri.y =
      (succ->tri.
       y >> (T8_DPRISM_MAXLEVEL - level + 1)) << (T8_DPRISM_MAXLEVEL - level +
                                                  1);
#endif
#if 1
    succ->line.x =
      (succ->line.
       x >> (T8_DPRISM_MAXLEVEL - level + 1)) << (T8_DPRISM_MAXLEVEL - level +
                                                  1);
#endif
    /*Set the level to the actual level */
    succ->line.level = level;
    succ->tri.level = level;
  }
  /*The next prism is one plane up, local_tri_id = 0 */
  else if ((prism_child_id + 1) % T8_DTRI_CHILDREN == 0) {
    /*parent is computed with succ, cause there are the updated datas */
    t8_dprism_parent (succ, succ);
    t8_dprism_child (succ, prism_child_id + 1, succ);
  }
  /*The next Prism is in the same plane, but has the next base-triangle */
  else {
    t8_dtri_successor (&p->tri, &succ->tri, level);
  }
  T8_ASSERT (succ->line.level == succ->tri.level);
}

void
t8_dprism_first_descendant (const t8_dprism_t * p, t8_dprism_t * s, int level)
{
  T8_ASSERT (level >= p->line.level && level <= T8_DPRISM_MAXLEVEL);
  T8_ASSERT (p->line.level == p->tri.level);
  /*First prism descendant = first triangle desc x first line desc */

  t8_dtri_first_descendant (&p->tri, &s->tri, level);
  t8_dline_first_descendant (&p->line, &s->line, level);

  T8_ASSERT (s->line.level == s->tri.level);
}

void
t8_dprism_last_descendant (const t8_dprism_t * p, t8_dprism_t * s, int level)
{
  T8_ASSERT (level >= p->line.level && level <= T8_DPRISM_MAXLEVEL);
  T8_ASSERT (p->line.level == p->tri.level);
  /*Last prism descendant = last triangle desc x last line desc */
  T8_ASSERT (level == T8_DTRI_MAXLEVEL);

  t8_dtri_last_descendant (&p->tri, &s->tri, level);
  t8_dline_last_descendant (&p->line, &s->line, level);

  T8_ASSERT (s->line.level == s->tri.level);
}

void
t8_dprism_vertex_coords (const t8_dprism_t * p, int vertex, int coords[3])
{
  T8_ASSERT (vertex >= 0 && vertex < 6);
  T8_ASSERT (p->line.level == p->tri.level);
  /*Compute x and y coordinate */
  t8_dtri_compute_coords (&p->tri, vertex % 3, coords);
  /*Compute z coordinate */
  t8_dline_vertex_coords (&p->line, vertex / 3, &coords[2]);
}

uint64_t
t8_dprism_linear_id (const t8_dprism_t * p, int level)
{
  uint64_t            id = 0;
  uint64_t            tri_id;
  uint64_t            line_id;
  int                 i;
  int                 prisms_of_size_i = 1;
  /*line_level = Num_of_Line_children ^ (level - 1) */
  int                 line_level;
  /*prism_shift = Num_of_Prism_children / 2 * 8 ^ (level - 1) */
  int                 prism_shift;

  T8_ASSERT (0 <= level && level <= T8_DPRISM_MAXLEVEL);
  T8_ASSERT (p->line.level == p->tri.level);
  /*id = 0 for root element */
  if (level == 0) {
    return 0;
  }
  line_level = sc_intpow (T8_DLINE_CHILDREN, level - 1);
  prism_shift =
    (T8_DPRISM_CHILDREN >> 1) * sc_intpow (T8_DPRISM_CHILDREN, level - 1);

  tri_id = t8_dtri_linear_id (&p->tri, level);
  line_id = t8_dline_linear_id (&p->line, level);
  for (i = 0; i < level; i++) {
    /*Compute via getting the local id of each ancestor triangle in which
     *elem->tri lies, the prism id, that elem would have, if it lies on the
     * lowest plane of the prism of level 0*/
    id += (tri_id % T8_DTRI_CHILDREN) * prisms_of_size_i;
    tri_id /= T8_DTRI_CHILDREN;
    prisms_of_size_i *= T8_DPRISM_CHILDREN;
  }
  /*Now add the actual plane in which the prism is, which is computed via
   * line_id*/
  for (i = level - 1; i >= 0; i--) {
    /*The number to add to the id computed via the tri_id is 4*8^(level-i)
     *for each plane in a prism of size i*/
    id += line_id / line_level * prism_shift;
    line_id = (uint64_t) (line_id % line_level);
    prism_shift /= T8_DPRISM_CHILDREN;
    line_level /= T8_DLINE_CHILDREN;
  }
  return id;
}