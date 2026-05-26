#define USE_THE_REPOSITORY_VARIABLE

#include "builtin.h"
#include "config.h"
#include "environment.h"
#include "parse-options.h"
#include "repository.h"
#include "revision.h"
#include "read-cache-ll.h"
#include "promisor-remote.h"
#include "path-walk.h"
#include "object.h"
#include "name-hash.h"
#include "oid-array.h"
#include "odb.h"
#include "packfile.h"

/*
 * Track candidate blobs that survived all three filters:
 * 1. Object type is OBJ_BLOB
 * 2. Inflated size >= min_size
 * 3. Object is not referenced by the current index (default) or
 *	not an already-promised object
 *
 * Store the inflated size so that we can report total reclaimable
 * space without another pass over the ODB.
 */
struct blob_candidate {
	struct object_id oid;
	unsigned long size; /* uncompressed (inflated) size of the tracked blob */
};

/* A growable collection tracking candidate objects targeted for deletion */
struct candidate_list {
	struct blob_candidate *candidates;
	size_t nr, alloc;
};

#define CANDIDATE_LIST_INIT { NULL, 0, 0 }

static void candidate_list_clear(struct candidate_list *list)
{
	free(list->candidates);
	list->candidates = NULL;
	list->nr = 0;
	list->alloc = 0;
}

/*
 * Append elements to the growable candidates array following Git's
 * standard ALLOC_GROW memory allocation convention.
 */

static void candidate_list_append(struct candidate_list *list,
				const struct object_id *oid,
				unsigned long size)
{
	ALLOC_GROW(list->candidates, list->nr + 1, list->alloc);
	oidcpy(&list->candidates[list->nr].oid, oid);
	list->candidates[list->nr].size = size;
	list->nr++;
}

/*
 * Drop only the blobs whose inflated size is >= min_size bytes
 * 0 = no size filter - consider all the blobs
 * user can set via --min-size=<bytes>
 *
 * Skip blobs whose oids are present in the current index
 * default = 1, 0 via --include-indexed
 */
struct enumeration_opts {
	unsigned long min_size;
	int skip_indexed;

	int verbose;
};

#define ENUMERATION_OPTS_INIT { 0, 1, 0 }

/*
 * Walk the current index once and collect every regular-file blob oid
 * into a sorted oid_array for O(log n) per-candidate lookup
 *
 * Submodule entries (S_ISGITLINK) and sparse directory entries
 * (S_ISSPARSEDIR) are skipped -their oids are not blob oids
 *
 */

static void build_index_oids(struct repository *repo, struct oid_array *out)
{
	struct index_state *istate = repo->index;
	unsigned int i;

	if (repo_read_index(repo) < 0)
		die(_("unable to read index"));

	for (i = 0; i < istate->cache_nr; i++) {
		struct cache_entry *ce;

		ce = istate->cache[i];
		if (S_ISGITLINK(ce->ce_mode))
			continue;
		if (S_ISSPARSEDIR(ce->ce_mode))
			continue;

		oid_array_append(out, &ce->oid);
	}

	/* oid_array_lookup() requires a sorted array for binary search optimization */
	oid_array_sort(out);
}

/*
 * Data structure to store enumeration context across callback loops
 */
struct enumeration_cb_data {
	struct repository *repo;
	const struct enumeration_opts *opts;
	struct oid_array *index_oids; /* NULL when skip_indexed == 0 */
	struct candidate_list *candidates;

	/*
	 * Diagnostics tracking for verbose output summaries
	 */
	unsigned long skipped_non_blob;
	unsigned long skipped_promisor;
	unsigned long skipped_small;
	unsigned long skipped_indexed;
	unsigned long errors;
};

/*
 * process_one_object - Apply  filters to an OID
 *
 * Process each object and verify that the object:
 * 1. Is not already tracked as a promisor object.
 * 2. Is a valid file blob.
 * 3. Satisfies the user-configured size limits.
 * 4. Is unreferenced by the current active index.
 *
 * Crucially, we always pass OBJECT_INFO_SKIP_FETCH_OBJECT to prevent local
 * size checks from triggering an expensive lazy-fetch storm over the network
 * from the promisor remote.
 *
 * remove_fetched_oids() in promisor-remote.c follows the same
 * pattern as this
 *
 * To optimize performance, the evaluation runs in two passes:
 * Pass 1: Quick type lookup via OBJECT_INFO_QUICK to discard commits,
 * trees, and tags
 * Pass 2: extract uncompressed size metrics for verified blobs
 * to match against '--min-size' limits
 *
 * The extra delta-expansion cost is saved with Pass 1
 */

static void process_one_object(struct enumeration_cb_data *cb_data,
			const struct object_id *oid)
{
	struct object_info oi = OBJECT_INFO_INIT;
	enum object_type type = OBJ_BAD;
	unsigned long size = 0;

	/*
	 * Promisor filter: objects already under the promisor contract
	 * should not be re-processed. is_promisor_object() uses an
	 * internal oidset populated once per process, so it is cheap
	 * after the first call.
	 *
	 * This catches objects in promisor packs that odb_for_each_object
	 * visits before we can filter at the pack level.
	 */

	if (is_promisor_object(cb_data->repo, oid)) {
		cb_data->skipped_promisor++;
		return;
	}

	/* Pass 1: type check */
	oi.typep = &type;
	oi.sizep = NULL;

	/*
	 * Object listed in pack index but unreadable. Can happen
	 * if another process is mid-repack. Count and skip rather
	 * than die() so a concurrent gc does not abort enumeration.
	 */
	if (odb_read_object_info_extended(cb_data->repo->objects, oid, &oi,
					  OBJECT_INFO_SKIP_FETCH_OBJECT | OBJECT_INFO_QUICK) < 0) {
		cb_data->errors++;
		return;
	}

	/* Filter 1: blobs only */
	if (type != OBJ_BLOB) {
		cb_data->skipped_non_blob++;
		return;
	}

	/* Pass 2: inflated size, blobs only */
	oi = (struct object_info) OBJECT_INFO_INIT;
	oi.sizep = &size;

	if (odb_read_object_info_extended(cb_data->repo->objects, oid, &oi,
					  OBJECT_INFO_SKIP_FETCH_OBJECT) < 0) {
		cb_data->errors++;
		return;
	}

	/* Filter 2: size threshold */
	if (cb_data->opts->min_size > 0 && size < cb_data->opts->min_size) {
		cb_data->skipped_small++;
		return;
	}

	/* Filter 3: skip indexed blobs if the option is set */
	if (cb_data->opts->skip_indexed && cb_data->index_oids) {
		if (oid_array_lookup(cb_data->index_oids, oid) >= 0) {
			cb_data->skipped_indexed++;
			return;
		}
	}

	candidate_list_append(cb_data->candidates, oid, size);
}
