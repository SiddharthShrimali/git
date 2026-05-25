#define USE_THE_REPOSITORY_VARIABLE

#include "builtin.h"
#include "config.h"
#include "environment.h"
#include "parse-options.h"
#include "repository.h"
#include "string-list.h"
#include "revision.h"
#include "promisor-remote.h"
#include "path-walk.h"

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
};

#define ENUMERATION_OPTS_INIT { 0 , 1 }
