/* Copyright (C) 2006 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "acl-cache.h"
#include "acl-api.h"

/* Give more than enough so that the arrays should never have to be grown.
   IMAP ACLs define only 10 standard rights and 10 user-defined rights. */
#define DEFAULT_ACL_RIGHTS_COUNT 64

struct acl_object_cache {
	char *name;

	struct acl_mask *my_rights[ACL_ID_TYPE_COUNT];
	struct acl_mask *my_neg_rights[ACL_ID_TYPE_COUNT];

	/* Needs to be calculated from my_*rights if NULL. */
	struct acl_mask *my_current_rights;
};

struct acl_cache {
	struct acl_backend *backend;
	struct hash_table *objects; /* name => struct acl_object_cache* */

	/* Right names mapping is used for faster rights checking. Note that
	   acl_mask bitmask relies on the order to never change, so only new
	   rights can be added to the mapping. */
	pool_t right_names_pool;
	/* idx => right name. */
	array_t ARRAY_DEFINE(right_idx_name_map, const char *);
	/* name => idx */
	struct hash_table *right_name_idx_map;
};

struct acl_cache *acl_cache_init(struct acl_backend *backend)
{
	struct acl_cache *cache;

	cache = i_new(struct acl_cache, 1);
	cache->backend = backend;
	cache->right_names_pool =
		pool_alloconly_create("ACL right names", 1024);
	cache->objects = hash_create(default_pool, default_pool, 0,
				     str_hash, (hash_cmp_callback_t *)strcmp);
	cache->right_name_idx_map =
		hash_create(default_pool, cache->right_names_pool, 0,
			    str_hash, (hash_cmp_callback_t *)strcmp);
	ARRAY_CREATE(&cache->right_idx_name_map, default_pool,
		     const char *, DEFAULT_ACL_RIGHTS_COUNT);
	return cache;
}

void acl_cache_deinit(struct acl_cache **_cache)
{
	struct acl_cache *cache = *_cache;

	*_cache = NULL;
	array_free(&cache->right_idx_name_map);
	hash_destroy(cache->right_name_idx_map);
	hash_destroy(cache->objects);
	pool_unref(cache->right_names_pool);
	i_free(cache);
}

static void acl_cache_free_object_cache(struct acl_object_cache *obj_cache)
{
	unsigned int i;

	for (i = 0; i < ACL_ID_TYPE_COUNT; i++) {
		if (obj_cache->my_rights[i] != NULL)
			acl_cache_mask_deinit(&obj_cache->my_rights[i]);
		if (obj_cache->my_neg_rights[i] != NULL)
			acl_cache_mask_deinit(&obj_cache->my_neg_rights[i]);
	}
	if (obj_cache->my_current_rights != NULL)
		acl_cache_mask_deinit(&obj_cache->my_current_rights);
	i_free(obj_cache->name);
	i_free(obj_cache);
}

struct acl_mask *acl_cache_mask_init(struct acl_cache *cache, pool_t pool,
				     const char *const *rights)
{
	struct acl_mask *mask;
	unsigned int rights_count, i, idx;
	unsigned char *p;
	buffer_t *bitmask;

	t_push();
	rights_count = strarray_length(rights);
	bitmask = buffer_create_dynamic(pool_datastack_create(),
					DEFAULT_ACL_RIGHTS_COUNT / CHAR_BIT);
	for (i = 0; i < rights_count; i++) {
		idx = acl_cache_right_lookup(cache, rights[i]);
		p = buffer_get_space_unsafe(bitmask, idx / CHAR_BIT, 1);
		*p |= 1 << (idx % CHAR_BIT);
	}

	/* @UNSAFE */
	mask = p_malloc(pool, SIZEOF_ACL_MASK(bitmask->used));
	memcpy(mask->mask, bitmask->data, bitmask->used);
	mask->pool = pool;
	mask->size = bitmask->used;
	t_pop();
	return mask;
}

void acl_cache_mask_deinit(struct acl_mask **_mask)
{
	struct acl_mask *mask = *_mask;

	*_mask = NULL;
	p_free(mask->pool, mask);
}

unsigned int acl_cache_right_lookup(struct acl_cache *cache, const char *right)
{
	unsigned int idx;
	void *idx_p;
	char *name;

	/* use +1 for right_name_idx_map values because we can't add NULL
	   values. */
	idx_p = hash_lookup(cache->right_name_idx_map, right);
	if (idx_p == NULL) {
		/* new right name, add it */
		name = p_strdup(cache->right_names_pool, right);

		idx = array_count(&cache->right_idx_name_map);
		array_append(&cache->right_idx_name_map, &name, 1);
		hash_insert(cache->right_name_idx_map, name,
			    POINTER_CAST(idx + 1));
	} else {
		idx = POINTER_CAST_TO(idx_p, unsigned int)-1;
	}
	return idx;
}

void acl_cache_flush(struct acl_cache *cache, const char *objname)
{
	struct acl_object_cache *obj_cache;

	obj_cache = hash_lookup(cache->objects, objname);
	if (obj_cache != NULL) {
		hash_remove(cache->objects, objname);
		acl_cache_free_object_cache(obj_cache);
	}
}

void acl_cache_flush_all(struct acl_cache *cache)
{
	struct hash_iterate_context *iter;
	void *key, *value;

	iter = hash_iterate_init(cache->objects);
	while (hash_iterate(iter, &key, &value)) {
		struct acl_object_cache *obj_cache = value;

		acl_cache_free_object_cache(obj_cache);
	}
	hash_iterate_deinit(iter);

	hash_clear(cache->objects, FALSE);
}

static void
acl_cache_update_rights_mask(struct acl_cache *cache,
			     struct acl_object_cache *obj_cache,
			     enum acl_modify_mode modify_mode,
			     const char *const *rights,
			     struct acl_mask **mask_p)
{
	struct acl_mask *change_mask, *old_mask, *new_mask;
	unsigned int i, size;
	bool changed = TRUE;

	change_mask = rights == NULL ? NULL :
		acl_cache_mask_init(cache, default_pool, rights);
	old_mask = *mask_p;
	new_mask = old_mask;

	switch (modify_mode) {
	case ACL_MODIFY_MODE_ADD:
		if (old_mask == NULL) {
			new_mask = change_mask;
			break;
		}

		if (change_mask == NULL) {
			/* no changes */
			changed = FALSE;
			break;
		}

		/* merge the masks */
		if (old_mask->size >= change_mask->size) {
			/* keep using the old mask */
			for (i = 0; i < change_mask->size; i++)
				old_mask->mask[i] |= change_mask->mask[i];
		} else {
			/* use the new mask, put old changes into it */
			for (i = 0; i < old_mask->size; i++)
				change_mask->mask[i] |= old_mask->mask[i];
			new_mask = change_mask;
		}
		break;
	case ACL_MODIFY_MODE_REMOVE:
		if (old_mask == NULL || change_mask == NULL) {
			changed = FALSE;
			break;
		}

		/* remove changed bits from old mask */
		size = I_MIN(old_mask->size, change_mask->size);
		for (i = 0; i < size; i++)
			old_mask->mask[i] &= ~change_mask->mask[i];
		break;
	case ACL_MODIFY_MODE_REPLACE:
		if (old_mask == NULL && change_mask == NULL)
			changed = FALSE;
		new_mask = change_mask;
		break;
	}

	if (new_mask != old_mask) {
		*mask_p = new_mask;
		if (old_mask != NULL)
			acl_cache_mask_deinit(&old_mask);
	}
	if (new_mask != change_mask && change_mask != NULL)
		acl_cache_mask_deinit(&change_mask);

	if (changed && obj_cache->my_current_rights != NULL) {
		/* current rights need to be recalculated */
		acl_cache_mask_deinit(&obj_cache->my_current_rights);
	}
}

static void
acl_cache_update_rights(struct acl_cache *cache,
			struct acl_object_cache *obj_cache,
			const struct acl_rights *rights)
{
	enum acl_id_type id_type = rights->id_type;

	acl_cache_update_rights_mask(cache, obj_cache, rights->modify_mode,
				     rights->rights,
				     &obj_cache->my_rights[id_type]);
	acl_cache_update_rights_mask(cache, obj_cache, rights->neg_modify_mode,
				     rights->neg_rights,
				     &obj_cache->my_neg_rights[id_type]);
}

void acl_cache_update(struct acl_cache *cache, const char *objname,
		      const struct acl_rights *rights)
{
	struct acl_object_cache *obj_cache;

	obj_cache = hash_lookup(cache->objects, objname);
	if (obj_cache == NULL) {
		obj_cache = i_new(struct acl_object_cache, 1);
		obj_cache->name = i_strdup(objname);
		hash_insert(cache->objects, obj_cache->name, obj_cache);
	}

	switch (rights->id_type) {
	case ACL_ID_ANYONE:
		acl_cache_update_rights(cache, obj_cache, rights);
		break;
	case ACL_ID_AUTHENTICATED:
		if (acl_backend_user_is_authenticated(cache->backend))
			acl_cache_update_rights(cache, obj_cache, rights);
		break;
	case ACL_ID_GROUP:
	case ACL_ID_GROUP_OVERRIDE:
		if (acl_backend_user_is_in_group(cache->backend,
						 rights->identifier))
			acl_cache_update_rights(cache, obj_cache, rights);
		break;
	case ACL_ID_USER:
		if (acl_backend_user_name_equals(cache->backend,
						 rights->identifier))
			acl_cache_update_rights(cache, obj_cache, rights);
		break;
	case ACL_ID_TYPE_COUNT:
		i_unreached();
	}
}

const char *const *acl_cache_get_names(struct acl_cache *cache,
				       unsigned int *count_r)
{
	*count_r = array_count(&cache->right_idx_name_map);
	return array_idx(&cache->right_idx_name_map, 0);
}

static void
acl_cache_my_current_rights_recalculate(struct acl_object_cache *obj_cache)
{
	struct acl_mask *mask;
	buffer_t *bitmask;
	unsigned char *p;
	unsigned int i, j, right_size;

	t_push();
	bitmask = buffer_create_dynamic(pool_datastack_create(),
					DEFAULT_ACL_RIGHTS_COUNT / CHAR_BIT);
	for (i = 0; i < ACL_ID_TYPE_COUNT; i++) {
		if (obj_cache->my_rights[i] != NULL) {
			/* apply the positive rights */
			right_size = obj_cache->my_rights[i]->size;
			p = buffer_get_space_unsafe(bitmask, 0, right_size);
			for (j = 0; j < right_size; j++)
				p[j] |= obj_cache->my_rights[i]->mask[j];
		}

		if (obj_cache->my_neg_rights[i] != NULL) {
			/* apply the negative rights. they override positive
			   rights. */
			right_size = obj_cache->my_neg_rights[i]->size;
			p = buffer_get_space_unsafe(bitmask, 0, right_size);
			for (j = 0; j < right_size; j++) {
				p[j] |=
					obj_cache->my_neg_rights[i]->mask[j];
			}
		}
	}

	/* @UNSAFE */
	obj_cache->my_current_rights = mask =
		i_malloc(SIZEOF_ACL_MASK(bitmask->used));
	memcpy(mask->mask, bitmask->data, bitmask->used);
	mask->pool = default_pool;
	mask->size = bitmask->used;
	t_pop();
}

const struct acl_mask *
acl_cache_get_my_rights(struct acl_cache *cache, const char *objname)
{
	struct acl_object_cache *obj_cache;

	obj_cache = hash_lookup(cache->objects, objname);
	if (obj_cache == NULL)
		return NULL;

	if (obj_cache->my_current_rights == NULL)
		acl_cache_my_current_rights_recalculate(obj_cache);
	return obj_cache->my_current_rights;
}
