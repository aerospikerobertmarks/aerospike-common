/*
 * Copyright 2008-2017 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */

//==========================================================
// Includes.
//

#include <citrusleaf/cf_rchash.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <citrusleaf/alloc.h>
#include <citrusleaf/cf_atomic.h>


//==========================================================
// Forward declarations.
//

// Variable key size public API.
int cf_rchash_put_v(cf_rchash *h, void *key, uint32_t key_size, void *object);
int cf_rchash_put_unique_v(cf_rchash *h, void *key, uint32_t key_size, void *object);
int cf_rchash_get_v(cf_rchash *h, void *key, uint32_t key_size, void **object_r);
int cf_rchash_delete_object_v(cf_rchash *h, void *key, uint32_t key_size, void *object);
int cf_rchash_reduce_v(cf_rchash *h, cf_rchash_reduce_fn reduce_fn, void *udata);

// Generic utilities.
static inline void cf_rchash_destroy_elements(cf_rchash *h);
static inline void cf_rchash_destroy_elements_v(cf_rchash *h);
static inline uint32_t cf_rchash_calculate_hash(cf_rchash *h, void *key, uint32_t key_size);
static inline pthread_mutex_t *cf_rchash_lock(cf_rchash *h, uint32_t hash);
static inline void cf_rchash_unlock(pthread_mutex_t *l);
static inline cf_rchash_elem_f *cf_rchash_get_bucket(cf_rchash *h, uint32_t i);
static inline cf_rchash_elem_v *cf_rchash_get_bucket_v(cf_rchash *h, uint32_t i);
static inline void cf_rchash_fill_element(cf_rchash_elem_f *e, cf_rchash *h, void *key, void *object);
static inline int cf_rchash_fill_element_v(cf_rchash_elem_v *e, cf_rchash *h, void *key, uint32_t key_size, void *object);
static inline void cf_rchash_size_incr(cf_rchash *h);
static inline void cf_rchash_size_decr(cf_rchash *h);
static inline void cf_rchash_release_object(cf_rchash *h, void *object);


//==========================================================
// Public API.
//

int
cf_rchash_create(cf_rchash **h_r, cf_rchash_hash_fn h_fn,
		cf_rchash_destructor_fn d_fn, uint32_t key_size, uint32_t n_buckets,
		uint32_t flags)
{
	if (! h_r || ! h_fn || n_buckets == 0) {
		return CF_RCHASH_ERR;
	}

	cf_rchash *h = cf_malloc(sizeof(cf_rchash));

	if (! h) {
		return CF_RCHASH_ERR;
	}

	h->h_fn = h_fn;
	h->d_fn = d_fn;
	h->key_size = key_size;
	h->n_buckets = n_buckets;
	h->flags = flags;
	h->n_elements = 0;

	// Can't have both lock options, but can opt for no locks at all.
	if ((flags & CF_RCHASH_CR_MT_BIGLOCK) != 0 &&
			(flags & CF_RCHASH_CR_MT_MANYLOCK) != 0) {
		cf_free(h);
		return CF_RCHASH_ERR;
	}

	// key_size == 0 always serves as flag to use variable key size public API.
	if (key_size == 0) {
		h->table = cf_calloc(n_buckets, sizeof(cf_rchash_elem_v));
	}
	else {
		h->table = cf_calloc(n_buckets, sizeof(cf_rchash_elem_f) + key_size);
	}

	if (! h->table) {
		cf_free(h);
		return CF_RCHASH_ERR;
	}

	if ((flags & CF_RCHASH_CR_MT_BIGLOCK) != 0) {
		pthread_mutex_init(&h->biglock, NULL);
	}
	else if ((flags & CF_RCHASH_CR_MT_MANYLOCK) != 0) {
		h->bucket_locks = cf_malloc(sizeof(pthread_mutex_t) * n_buckets);

		if (! h->bucket_locks) {
			cf_free(h);
			return CF_RCHASH_ERR;
		}

		for (uint32_t i = 0; i < n_buckets; i++) {
			pthread_mutex_init(&h->bucket_locks[i], NULL);
		}
	}

	*h_r = h;

	return CF_RCHASH_OK;
}


void
cf_rchash_destroy(cf_rchash *h)
{
	if (! h) {
		return;
	}

	if (h->key_size == 0) {
		cf_rchash_destroy_elements_v(h);
	}
	else {
		cf_rchash_destroy_elements(h);
	}

	if ((h->flags & CF_RCHASH_CR_MT_BIGLOCK) != 0) {
		pthread_mutex_destroy(&h->biglock);
	}
	else if ((h->flags & CF_RCHASH_CR_MT_MANYLOCK) != 0) {
		for (uint32_t i = 0; i < h->n_buckets; i++) {
			pthread_mutex_destroy(&h->bucket_locks[i]);
		}

		cf_free(h->bucket_locks);
	}

	cf_free(h->table);
	cf_free(h);
}


uint32_t
cf_rchash_get_size(cf_rchash *h)
{
	// For now, not bothering with different methods per lock mode.
	return cf_atomic32_get(h->n_elements);
}


// If key is not already in hash, insert it with specified rc_malloc'd object.
// If key is already in hash, replace (and release) existing object.
int
cf_rchash_put(cf_rchash *h, void *key, uint32_t key_size, void *object)
{
	if (! h || ! key || ! object) {
		return CF_RCHASH_ERR;
	}

	if (h->key_size == 0) {
		return cf_rchash_put_v(h, key, key_size, object);
	}

	if (h->key_size != key_size) {
		return CF_RCHASH_ERR;
	}

	uint32_t hash = cf_rchash_calculate_hash(h, key, key_size);
	pthread_mutex_t *l = cf_rchash_lock(h, hash);
	cf_rchash_elem_f *e = cf_rchash_get_bucket(h, hash);

	// Most common case should be insert into empty bucket.
	if (! e->object) {
		cf_rchash_fill_element(e, h, key, object);
		cf_rchash_unlock(l);
		return CF_RCHASH_OK;
	}

	cf_rchash_elem_f *e_head = e;

	while (e) {
		if (memcmp(e->key, key, key_size) != 0) {
			e = e->next;
			continue;
		}

		// In this case we're replacing the previous object with the new object.
		void *free_object = e->object;

		e->object = object;

		cf_rchash_unlock(l);
		cf_rchash_release_object(h, free_object);

		return CF_RCHASH_OK;
	}

	e = (cf_rchash_elem_f *)cf_malloc(sizeof(cf_rchash_elem_f) + key_size);

	if (! e) {
		cf_rchash_unlock(l);
		return CF_RCHASH_ERR;
	}

	cf_rchash_fill_element(e, h, key, object);

	// Insert just after head.
	e->next = e_head->next;
	e_head->next = e;

	cf_rchash_unlock(l);

	return CF_RCHASH_OK;
}


// Like cf_rchash_put(), but if key is already in hash, fail.
int
cf_rchash_put_unique(cf_rchash *h, void *key, uint32_t key_size, void *object)
{
	if (! h || ! key || ! object) {
		return CF_RCHASH_ERR;
	}

	if (h->key_size == 0) {
		return cf_rchash_put_unique_v(h, key, key_size, object);
	}

	if (h->key_size != key_size) {
		return CF_RCHASH_ERR;
	}

	uint32_t hash = cf_rchash_calculate_hash(h, key, key_size);
	pthread_mutex_t *l = cf_rchash_lock(h, hash);
	cf_rchash_elem_f *e = cf_rchash_get_bucket(h, hash);

	// Most common case should be insert into empty bucket.
	if (! e->object) {
		cf_rchash_fill_element(e, h, key, object);
		cf_rchash_unlock(l);
		return CF_RCHASH_OK;
	}

	cf_rchash_elem_f *e_head = e;

	// Check for uniqueness of key - if not unique, fail!
	while (e) {
		if (memcmp(e->key, key, key_size) == 0) {
			cf_rchash_unlock(l);
			return CF_RCHASH_ERR_FOUND;
		}

		e = e->next;
	}

	e = (cf_rchash_elem_f *)cf_malloc(sizeof(cf_rchash_elem_f) + key_size);

	if (! e) {
		cf_rchash_unlock(l);
		return CF_RCHASH_ERR;
	}

	cf_rchash_fill_element(e, h, key, object);

	// Insert just after head.
	e->next = e_head->next;
	e_head->next = e;

	cf_rchash_unlock(l);

	return CF_RCHASH_OK;
}


// If key is found, object is returned with extra ref-count. When finished with
// it, caller must always release the returned object, and must destroy and free
// the object if the ref-count hits 0 - i.e. caller should do the equivalent of
// cf_rchash_release_object().
//
// Or, caller may pass NULL object_r to use this method as an existence check.
int
cf_rchash_get(cf_rchash *h, void *key, uint32_t key_size, void **object_r)
{
	if (! h || ! key) {
		return CF_RCHASH_ERR;
	}

	if (h->key_size == 0) {
		return cf_rchash_get_v(h, key, key_size, object_r);
	}

	if (h->key_size != key_size) {
		return CF_RCHASH_ERR;
	}

	uint32_t hash = cf_rchash_calculate_hash(h, key, key_size);
	pthread_mutex_t *l = cf_rchash_lock(h, hash);
	cf_rchash_elem_f *e = cf_rchash_get_bucket(h, hash);

	// Finding an empty bucket means key is not here.
	if (! e->object) {
		cf_rchash_unlock(l);
		return CF_RCHASH_ERR_NOTFOUND;
	}

	while (e) {
		if (memcmp(key, e->key, key_size) != 0) {
			e = e->next;
			continue;
		}

		if (object_r) {
			cf_rc_reserve(e->object);
			*object_r = e->object;
		}

		cf_rchash_unlock(l);

		return CF_RCHASH_OK;
	}

	cf_rchash_unlock(l);

	return CF_RCHASH_ERR_NOTFOUND;
}


// Removes the key and object from the hash, releasing the "original" ref-count.
// If this causes the ref-count to hit 0, the object destructor is called and
// the object is freed.
int
cf_rchash_delete(cf_rchash *h, void *key, uint32_t key_size)
{
	// No check to verify the object.
	return cf_rchash_delete_object(h, key, key_size, NULL);
}


// Like cf_rchash_delete() but checks that object found matches that specified.
// Threads may race to delete and release the same object - they may be doing a
// typical get ... delete, release sequence, or a reduce that deletes. While
// ref-counts ensure only the *last* release destroys the object, the *first*
// delete removes the object from the hash. If a new object is then immediately
// inserted with the same key, other threads' deletes would mistakenly remove
// this new element from the hash if they do not verify the object.
int
cf_rchash_delete_object(cf_rchash *h, void *key, uint32_t key_size,
		void *object)
{
	if (! h || ! key) {
		return CF_RCHASH_ERR;
	}

	if (h->key_size == 0) {
		return cf_rchash_delete_object_v(h, key, key_size, object);
	}

	if (h->key_size != key_size) {
		return CF_RCHASH_ERR;
	}

	uint32_t hash = cf_rchash_calculate_hash(h, key, key_size);
	pthread_mutex_t *l = cf_rchash_lock(h, hash);
	cf_rchash_elem_f *e = cf_rchash_get_bucket(h, hash);

	if (! e->object) {
		cf_rchash_unlock(l);
		return CF_RCHASH_ERR_NOTFOUND;
	}

	cf_rchash_elem_f *e_prev = NULL;

	// Look for the element, remove and release if found.
	while (e) {
		if (memcmp(e->key, key, key_size) != 0) {
			e_prev = e;
			e = e->next;
			continue;
		}
		// else - found it, remove from hash and release outside lock...

		// ... unless it's the wrong object.
		if (object && object != e->object) {
			cf_rchash_unlock(l);
			return CF_RCHASH_ERR_NOTFOUND;
		}

		// Save pointers to release & free.
		void *free_object = e->object;
		cf_rchash_elem_f *free_e = NULL;

		// Patch pointers & free element if not head.
		if (e_prev) {
			e_prev->next = e->next;
			free_e = e;
		}
		// Am at head - more complicated.
		else {
			if (! e->next) {
				memset(e, 0, sizeof(cf_rchash_elem_f));
			}
			else {
				free_e = e->next;
				memcpy(e, e->next, sizeof(cf_rchash_elem_f) + key_size);
			}
		}

		cf_rchash_size_decr(h);
		cf_rchash_unlock(l);

		cf_rchash_release_object(h, free_object);

		if (free_e) {
			cf_free(free_e);
		}

		return CF_RCHASH_OK;
	}

	cf_rchash_unlock(l);

	return CF_RCHASH_ERR_NOTFOUND;
}


// Call the given function (reduce_fn) for every element in the tree.
//
// The value returned by reduce_fn governs behavior as follows:
// - CF_RCHASH_OK - continue iterating
// - CF_RCHASH_REDUCE_DELETE - delete the current element, continue iterating
// - anything else (e.g. CF_RCHASH_ERR) - stop iterating and return reduce_fn's
//   returned value
//
// If deleting an element causes the object ref-count to hit 0, the object
// destructor is called and the object is freed.
int
cf_rchash_reduce(cf_rchash *h, cf_rchash_reduce_fn reduce_fn, void *udata)
{
	if (! h || ! reduce_fn) {
		return CF_RCHASH_ERR;
	}

	if (h->key_size == 0) {
		return cf_rchash_reduce_v(h, reduce_fn, udata);
	}

	if ((h->flags & CF_RCHASH_CR_MT_BIGLOCK) != 0) {
		pthread_mutex_lock(&h->biglock);
	}

	for (uint32_t i = 0; i < h->n_buckets; i++) {
		pthread_mutex_t *bucket_lock = NULL;

		if ((h->flags & CF_RCHASH_CR_MT_MANYLOCK) != 0) {
			bucket_lock = &h->bucket_locks[i];
			pthread_mutex_lock(bucket_lock);
		}

		cf_rchash_elem_f *e = cf_rchash_get_bucket(h, i);
		cf_rchash_elem_f *e_prev = NULL;

		while (e) {
			if (! e->object) {
				// Nothing (more) in this row.
				break;
			}

			int rv = reduce_fn(e->key, h->key_size, e->object,
					udata);

			if (rv == CF_RCHASH_OK) {
				// Most common case - keep going.
				e_prev = e;
				e = e->next;
			}
			else if (rv == CF_RCHASH_REDUCE_DELETE) {
				cf_rchash_release_object(h, e->object);
				cf_rchash_size_decr(h);

				if (e_prev) {
					e_prev->next = e->next;
					cf_free(e);
					e = e_prev->next;
				}
				else {
					// At head with no next.
					if (! e->next) {
						memset(e, 0, sizeof(cf_rchash_elem_f));
						e = NULL;
					}
					// At head with a next. Copy next into current and free
					// next. e_prev stays NULL, e stays unchanged.
					else {
						cf_rchash_elem_f *free_e = e->next;

						memcpy(e, e->next,
								sizeof(cf_rchash_elem_f) + h->key_size);
						cf_free(free_e);
					}
				}
			}
			else {
				// Stop iterating.
				if (bucket_lock) {
					pthread_mutex_unlock(bucket_lock);
				}

				if ((h->flags & CF_RCHASH_CR_MT_BIGLOCK) != 0) {
					pthread_mutex_unlock(&h->biglock);
				}

				return rv;
			}
		}

		if (bucket_lock) {
			pthread_mutex_unlock(bucket_lock);
		}
	}

	if ((h->flags & CF_RCHASH_CR_MT_BIGLOCK) != 0) {
		pthread_mutex_unlock(&h->biglock);
	}

	return CF_RCHASH_OK;
}


//==========================================================
// Local helpers - variable key size public API.
//

int
cf_rchash_put_v(cf_rchash *h, void *key, uint32_t key_size, void *object)
{
	if (key_size == 0) {
		return CF_RCHASH_ERR;
	}

	uint32_t hash = cf_rchash_calculate_hash(h, key, key_size);
	pthread_mutex_t *l = cf_rchash_lock(h, hash);
	cf_rchash_elem_v *e = cf_rchash_get_bucket_v(h, hash);

	// Most common case should be insert into empty bucket.
	if (! e->object) {
		int rv = cf_rchash_fill_element_v(e, h, key, key_size, object);

		cf_rchash_unlock(l);
		return rv;
	}

	cf_rchash_elem_v *e_head = e;

	while (e) {
		if (key_size != e->key_size || memcmp(e->key, key, key_size) != 0) {
			e = e->next;
			continue;
		}

		// In this case we're replacing the previous object with the new object.
		void *free_object = e->object;

		e->object = object;

		cf_rchash_unlock(l);
		cf_rchash_release_object(h, free_object);

		return CF_RCHASH_OK;
	}

	e = (cf_rchash_elem_v *)cf_malloc(sizeof(cf_rchash_elem_v));

	if (! e) {
		cf_rchash_unlock(l);
		return CF_RCHASH_ERR;
	}

	if (cf_rchash_fill_element_v(e, h, key, key_size, object) != CF_RCHASH_OK) {
		cf_free(e);
		cf_rchash_unlock(l);
		return CF_RCHASH_ERR;
	}

	// Insert just after head.
	e->next = e_head->next;
	e_head->next = e;

	cf_rchash_unlock(l);

	return CF_RCHASH_OK;
}


int
cf_rchash_put_unique_v(cf_rchash *h, void *key, uint32_t key_size, void *object)
{
	if (key_size == 0) {
		return CF_RCHASH_ERR;
	}

	uint32_t hash = cf_rchash_calculate_hash(h, key, key_size);
	pthread_mutex_t *l = cf_rchash_lock(h, hash);
	cf_rchash_elem_v *e = cf_rchash_get_bucket_v(h, hash);

	// Most common case should be insert into empty bucket.
	if (! e->object) {
		int rv = cf_rchash_fill_element_v(e, h, key, key_size, object);

		cf_rchash_unlock(l);
		return rv;
	}

	cf_rchash_elem_v *e_head = e;

	// Check for uniqueness of key - if not unique, fail!
	while (e) {
		if (key_size == e->key_size && memcmp(e->key, key, key_size) == 0) {
			cf_rchash_unlock(l);
			return CF_RCHASH_ERR_FOUND;
		}

		e = e->next;
	}

	e = (cf_rchash_elem_v *)cf_malloc(sizeof(cf_rchash_elem_v));

	if (! e) {
		cf_rchash_unlock(l);
		return CF_RCHASH_ERR;
	}

	if (cf_rchash_fill_element_v(e, h, key, key_size, object) != CF_RCHASH_OK) {
		cf_free(e);
		cf_rchash_unlock(l);
		return CF_RCHASH_ERR;
	}

	// Insert just after head.
	e->next = e_head->next;
	e_head->next = e;

	cf_rchash_unlock(l);

	return CF_RCHASH_OK;
}


int
cf_rchash_get_v(cf_rchash *h, void *key, uint32_t key_size, void **object_r)
{
	if (key_size == 0) {
		return CF_RCHASH_ERR;
	}

	uint32_t hash = cf_rchash_calculate_hash(h, key, key_size);
	pthread_mutex_t *l = cf_rchash_lock(h, hash);
	cf_rchash_elem_v *e = cf_rchash_get_bucket_v(h, hash);

	if (! e->object) {
		cf_rchash_unlock(l);
		return CF_RCHASH_ERR_NOTFOUND;
	}

	while (e) {
		if (key_size != e->key_size || memcmp(key, e->key, key_size) != 0) {
			e = e->next;
			continue;
		}

		if (object_r) {
			cf_rc_reserve(e->object);
			*object_r = e->object;
		}

		cf_rchash_unlock(l);

		return CF_RCHASH_OK;
	}

	cf_rchash_unlock(l);

	return CF_RCHASH_ERR_NOTFOUND;
}


int
cf_rchash_delete_object_v(cf_rchash *h, void *key, uint32_t key_size,
		void *object)
{
	if (key_size == 0) {
		return CF_RCHASH_ERR;
	}

	uint32_t hash = cf_rchash_calculate_hash(h, key, key_size);
	pthread_mutex_t *l = cf_rchash_lock(h, hash);
	cf_rchash_elem_v *e = cf_rchash_get_bucket_v(h, hash);

	if (! e->object) {
		cf_rchash_unlock(l);
		return CF_RCHASH_ERR_NOTFOUND;
	}

	cf_rchash_elem_v *e_prev = NULL;

	// Look for the element, remove and release if found.
	while (e) {
		if (key_size != e->key_size || memcmp(e->key, key, key_size) != 0) {
			e_prev = e;
			e = e->next;
			continue;
		}
		// else - found it, remove from hash and release outside lock...

		// ... unless it's the wrong object.
		if (object && object != e->object) {
			cf_rchash_unlock(l);
			return CF_RCHASH_ERR_NOTFOUND;
		}

		// Save pointers to release & free.
		void *free_key = e->key;
		void *free_object = e->object;
		cf_rchash_elem_v *free_e = NULL;

		// Patch pointers & free element if not head.
		if (e_prev) {
			e_prev->next = e->next;
			free_e = e;
		}
		// Am at head - more complicated.
		else {
			if (! e->next) {
				memset(e, 0, sizeof(cf_rchash_elem_v));
			}
			else {
				free_e = e->next;
				memcpy(e, e->next, sizeof(cf_rchash_elem_v));
			}
		}

		cf_rchash_size_decr(h);
		cf_rchash_unlock(l);

		cf_free(free_key);
		cf_rchash_release_object(h, free_object);

		if (free_e) {
			cf_free(free_e);
		}

		return CF_RCHASH_OK;
	}

	cf_rchash_unlock(l);

	return CF_RCHASH_ERR_NOTFOUND;
}


int
cf_rchash_reduce_v(cf_rchash *h, cf_rchash_reduce_fn reduce_fn, void *udata)
{
	if ((h->flags & CF_RCHASH_CR_MT_BIGLOCK) != 0) {
		pthread_mutex_lock(&h->biglock);
	}

	for (uint32_t i = 0; i < h->n_buckets; i++) {
		pthread_mutex_t *bucket_lock = NULL;

		if ((h->flags & CF_RCHASH_CR_MT_MANYLOCK) != 0) {
			bucket_lock = &h->bucket_locks[i];
			pthread_mutex_lock(bucket_lock);
		}

		cf_rchash_elem_v *e = cf_rchash_get_bucket_v(h, i);
		cf_rchash_elem_v *e_prev = NULL;

		while (e) {
			if (e->key_size == 0) {
				// Nothing (more) in this row.
				break;
			}

			int rv = reduce_fn(e->key, e->key_size, e->object,
					udata);

			if (rv == CF_RCHASH_OK) {
				// Most common case - keep going.
				e_prev = e;
				e = e->next;
			}
			else if (rv == CF_RCHASH_REDUCE_DELETE) {
				cf_free(e->key);
				cf_rchash_release_object(h, e->object);

				cf_rchash_size_decr(h);

				if (e_prev) {
					e_prev->next = e->next;
					cf_free(e);
					e = e_prev->next;
				}
				else {
					// At head with no next.
					if (! e->next) {
						memset(e, 0, sizeof(cf_rchash_elem_v));
						e = NULL;
					}
					// At head with a next. Copy next into current and free
					// next. e_prev stays NULL, e stays unchanged.
					else {
						cf_rchash_elem_v *free_e = e->next;

						memcpy(e, e->next, sizeof(cf_rchash_elem_v));
						cf_free(free_e);
					}
				}
			}
			else {
				// Stop iterating.
				if (bucket_lock) {
					pthread_mutex_unlock(bucket_lock);
				}

				if ((h->flags & CF_RCHASH_CR_MT_BIGLOCK) != 0) {
					pthread_mutex_unlock(&h->biglock);
				}

				return rv;
			}
		}

		if (bucket_lock) {
			pthread_mutex_unlock(bucket_lock);
		}
	}

	if ((h->flags & CF_RCHASH_CR_MT_BIGLOCK) != 0) {
		pthread_mutex_unlock(&h->biglock);
	}

	return CF_RCHASH_OK;
}


//==========================================================
// Local helpers - generic utilities.
//

static inline void
cf_rchash_destroy_elements(cf_rchash *h)
{
	for (uint32_t i = 0; i < h->n_buckets; i++) {
		cf_rchash_elem_f *e = cf_rchash_get_bucket(h, i);

		if (! e->object) {
			continue;
		}

		cf_rchash_release_object(h, e->object);
		e = e->next; // skip the first, it's in place

		while (e) {
			cf_rchash_elem_f *temp = e->next;

			cf_rchash_release_object(h, e->object);
			cf_free(e);
			e = temp;
		}
	}

	h->n_elements = 0;
}


static inline void
cf_rchash_destroy_elements_v(cf_rchash *h)
{
	for (uint32_t i = 0; i < h->n_buckets; i++) {
		cf_rchash_elem_v *e = cf_rchash_get_bucket_v(h, i);

		if (! e->object) {
			continue;
		}

		cf_rchash_release_object(h, e->object);
		cf_free(e->key);
		e = e->next; // skip the first, it's in place

		while (e) {
			cf_rchash_elem_v *temp = e->next;

			cf_rchash_release_object(h, e->object);
			cf_free(e->key);
			cf_free(e);
			e = temp;
		}
	}
}


static inline uint32_t
cf_rchash_calculate_hash(cf_rchash *h, void *key, uint32_t key_size)
{
	return h->h_fn(key, key_size) % h->n_buckets;
}


static inline pthread_mutex_t *
cf_rchash_lock(cf_rchash *h, uint32_t hash)
{
	pthread_mutex_t *l = NULL;

	if ((h->flags & CF_RCHASH_CR_MT_BIGLOCK) != 0) {
		l = &h->biglock;
	}
	else if ((h->flags & CF_RCHASH_CR_MT_MANYLOCK) != 0) {
		l = &h->bucket_locks[hash];
	}

	if (l) {
		pthread_mutex_lock(l);
	}

	return l;
}


static inline void
cf_rchash_unlock(pthread_mutex_t *l)
{
	if (l) {
		pthread_mutex_unlock(l);
	}
}


static inline cf_rchash_elem_f *
cf_rchash_get_bucket(cf_rchash *h, uint32_t i)
{
	return (cf_rchash_elem_f *)((uint8_t *)h->table +
			((sizeof(cf_rchash_elem_f) + h->key_size) * i));
}


static inline cf_rchash_elem_v *
cf_rchash_get_bucket_v(cf_rchash *h, uint32_t i)
{
	return (cf_rchash_elem_v *)((uint8_t *)h->table +
			(sizeof(cf_rchash_elem_v) * i));
}


static inline void
cf_rchash_fill_element(cf_rchash_elem_f *e, cf_rchash *h, void *key,
		void *object)
{
	memcpy(e->key, key, h->key_size);
	e->object = object;
	cf_rchash_size_incr(h);
}


static inline int
cf_rchash_fill_element_v(cf_rchash_elem_v *e, cf_rchash *h, void *key,
		uint32_t key_size, void *object)
{
	e->key = cf_malloc(key_size);

	if (! e->key) {
		return CF_RCHASH_ERR;
	}

	memcpy(e->key, key, key_size);
	e->key_size = key_size;

	e->object = object;

	cf_rchash_size_incr(h);

	return CF_RCHASH_OK;
}


static inline void
cf_rchash_size_incr(cf_rchash *h)
{
	// For now, not bothering with different methods per lock mode.
	cf_atomic32_incr(&h->n_elements);
}


static inline void
cf_rchash_size_decr(cf_rchash *h)
{
	// For now, not bothering with different methods per lock mode.
	cf_atomic32_decr(&h->n_elements);
}


static inline void
cf_rchash_release_object(cf_rchash *h, void *object)
{
	if (cf_rc_release(object) == 0) {
		if (h->d_fn) {
			(h->d_fn)(object);
		}

		cf_rc_free(object);
	}
}
