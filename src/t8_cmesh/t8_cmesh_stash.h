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

/** \file t8_cmesh_stash.h
 * We define the data structures and routines for temporary storage before commit
 */

#ifndef T8_CMESH_STASH_H
#define T8_CMESH_STASH_H

#include <t8.h>
#include <t8_eclass.h>

typedef struct t8_stash *t8_stash_t;

typedef struct t8_stash_class
{
  t8_gloidx_t         id;     /**< The global tree id */
  t8_eclass_t         eclass; /**< The eclass of that tree */
} t8_stash_class_struct_t;

typedef struct t8_stash_joinface
{
  t8_gloidx_t         id1; /**< The global tree id of the first tree in the connection. */
  t8_gloidx_t         id2; /**< The global tree id of the second tree. */
  int                 face1; /**< The face number of the first of the connected faces. */
  int                 face2; /**< The face number of the second face. */
  int                 orientation; /**< The orientation of the face connection. \see t8_cmesh_types.h. */
} t8_stash_joinface_struct_t;

typedef struct t8_stash_attribute
{
  t8_gloidx_t         id;   /**< The global tree id */
  size_t              attr_size; /**< The size (in bytes) of this attribute */
  void               *attr_data; /**< Array of \a size bytes storing the attributes data. */
  int                 is_owned; /**< True if the data was copied, false if the data is still owned by user. */
  int                 package_id; /**< The id of the package that set this attribute. */
  int                 key; /**< The key used by the package to identify this attribute. */
} t8_stash_attribute_struct_t;

typedef struct t8_stash
{
  sc_array_t          classes; /**< Stores the eclasses of the trees. */
  sc_array_t          joinfaces; /**< Stores the face-connections. */
  sc_array_t          attributes; /**< Stores the attributes. */
} t8_stash_struct_t;

T8_EXTERN_C_BEGIN ();

/** Initialize a stash data structure.
 * \param [in,out]  pstash  A pointer to the stash to be initialized.
 */
void                t8_stash_init (t8_stash_t * pstash);

/** Free all memory associated in a stash structure.
 * \param [in,out]  pstash  A pointer to the stash to be destroyed.
 *                  The pointer is set to NULL after the function call.
 */
void                t8_stash_destroy (t8_stash_t * pstash);

/** Set the eclass of a tree.
 * \param [in, out] stash The stash to be updated.
 * \param [in]      id    The global id of the tree whose eclass should be set.
 * \param [in]      eclass  The eclass of tree with id \a id.
 */
void                t8_stash_add_class (t8_stash_t stash, t8_gloidx_t id,
                                        t8_eclass_t eclass);

/** Add a face connection to a stash.
 * \param [in, out] stash The stash to be updated.
 * \param [in]      id1   The global id of the first tree.
 * \param [in]      id2   The global id of the second tree,
 * \param [in]      face1 The face number of the face of the first tree.
 * \param [in]      face2 The face number of the face of the second tree.
 * \param [in]      orientation The orientation of the faces to each other.
 */
void                t8_stash_add_facejoin (t8_stash_t stash, t8_gloidx_t id1,
                                           t8_gloidx_t id2, int face1,
                                           int face2, int orientation);

/** Add an attribute to a tree.
 * \param [in] stash    The stash structure to be modified.
 * \param [in] id       The global index of the tree to which the attribute is added.
 * \param [in] package_id The unique id of the current package.
 * \param [in] key      An integer value used to identify this attribute.
 * \param [in] size     The size (in bytes) of the attribute.
 * \param [in] attr     Points to \a size bytes of memory that should be stored as the attribute.
 * \param [in] copy     If true the attribute data is copied from \a attr to an internal storage.
 *                      If false only the pointer \a attr is stored and the data is only copied
 *                      if the cmesh is committed. (More memory efficient).
 */
void                t8_stash_add_attribute (t8_stash_t stash, t8_gloidx_t id,
                                            int package_id, int key,
                                            size_t size, void *attr,
                                            int copy);

/** Return the size (in bytes) of an attribute in the stash.
 * \param [in]   stash   The stash to be considered.
 * \param [in]   index   The index of the attribute in the attribute array of \a stash.
 * \return               The size in bytes of the attribute.
 */
size_t              t8_stash_get_attribute_size (t8_stash_t stash,
                                                 size_t index);

/** Return the pointer to an attribute in the stash.
 * \param [in]   stash   The stash to be considered.
 * \param [in]   index   The index of the attribute in the attribute array of \a stash.
 * \return               A void pointer to the memory region where the attribute is stored.
 */
void               *t8_stash_get_attribute (t8_stash_t stash, size_t index);

/** Return the id of the tree a given attribute belongs to.
 * \param [in]   stash   The stash to be considered.
 * \param [in]   index   The index of the attribute in the attribute array of \a stash.
 * \return               The tree id.
 */
t8_gloidx_t         t8_stash_get_attribute_tree_id (t8_stash_t stash,
                                                    size_t index);

/** Return true if an attribute in the stash is owned by the stash, that is
 * it was copied in the call to \a t8_stash_add_attribute.
 * Returns false if the attribute is not owned by the stash.
 * \param [in]   stash   The stash to be considered.
 * \param [in]   index   The index of the attribute in the attribute array of \a stash.
 * \return               True of false.
 */
int                 t8_stash_attribute_is_owned (t8_stash_t stash,
                                                 size_t index);

/** Sort the attributes array of a stash in the order
 * (treeid, packageid, key) *
 * \param [in]   stash   The stash to be considered.
 */
void                t8_stash_attribute_sort (t8_stash_t stash);

/* broadcast the data of a stash on proc root.
 * stash is setup on root. on the other procs only stash_init was called
 * elem_counts holds number of attributes/classes/joinfaces
 */
t8_stash_t          t8_stash_bcast (t8_stash_t stash, int root,
                                    sc_MPI_Comm comm, size_t elem_counts[]);

/** Check two stashes for equal content and return true if so.
 * \param [in]   stash_a  The first stash to be considered.
 * \param [in]   stash_b  The first stash to be considered.
 * \return                True if both stashes hold copies of the same data.
 *                        False otherwise.
 */
int                 t8_stash_is_equal (t8_stash_t stash_a,
                                       t8_stash_t stash_b);

T8_EXTERN_C_END ();

#endif /* !T8_CMESH_STASH_H */
