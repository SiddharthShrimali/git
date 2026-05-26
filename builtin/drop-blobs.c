#define USE_THE_REPOSITORY_VARIABLE

#include "builtin.h"
#include "config.h"
#include "cache.h"
#include "environment.h"
#include "parse-options.h"
#include "repository.h"
#include "string-list.h"
#include "revision.h"
#include "read-cache-ll.h"
#include "promisor-remote.h"
#include "path-walk.h"
#include "object.h"
#include "name-hash.h"
#include "oid-array.h"
#include "object-store-ll.h"

/*
 * storing candidate blob that survived all three filters:
 *   1 - type = OBJ_BLOB
 *   2 - inflated size >= min_size
 *   3 - not referenced by the current index (default) or
 *	 not an already-promised object
 *
 * store the inflated size so that we can report total reclaimable
 * space without a second pass.
 */

struct blob_candidate {
	struct object_id oid;
	unsigned long size; /* storing the size of the blob to
				avoid parsing it later (inflated size) */
};

struct candidate_list {
	struct blob_candidate *candidates;
	size_t nr, alloc;
}; // a list of suitable candidates

#define CANDIDATE_LIST_INIT { NULL, 0, 0 }

static void candidate_list_clear(struct candidate_list *list) {
	free(list->candidates);
	list->candidates = NULL;
	list->nr = 0;
	list->alloc = 0;
}

/*
 * growable array of candidates. Follows Git's ALLOC_GROW convention
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
 * drop only the blobs whose inflated size is >= min_size bytes
 * 0 = no size filter - consider all the blobs
 * user can set via --min-size=<bytes>
 *
 * skip blobs whose oids are present in the current index
 * default: 1. 0 via --include-indexed
 */
struct enumeration_opts {
	unsigned long min_size;
	int skip_indexed;

	int verbose;
};

#define ENUMERATION_OPTS_INIT { 0 , 1 , 0}

/*
 * walk the current index once and collect every regular-file blob oid
 * into a sorted oid_array for O(log n) per-candidate lookup
 *
 * submodule entries (S_ISGITLINK) and sparse directory entries
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
		if(S_ISGITLINK(ce->ce_mode))
			continue;
		if (S_ISSPARSEDIR(ce->ce_mode))
			continue;

		oid_array_append(out, &ce->oid);
	}

	/* oid_array_lookup() requires a sorted array */
	oid_array_sort(out);
};

/*
 * data structure to store enumeration callback data
 * since the odb_for_each_oid return 0 or 1
 */

struct enumeration_cb_data {
	struct repository *repo;
	const struct enumeration_opts *opts;
	const struct oid_array *index_oids; // null when skip_indexed =0
	struct candidate_list *candidates;

	/*
	 * diagnostics for verbose to return info abouut enumeration data
	 */
	unsigned long skipped_non_blob;
	unsigned long skipped_promisor;
	unsigned long skipped_small;
	unsigned long skipped_indexed;
	unsigned long errors;
};
