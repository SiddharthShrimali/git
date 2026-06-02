#include "git-compat-util.h"
#include "repack.h"
#include "repository.h"
#include "run-command.h"
#include "string-list.h"
#include "hex.h"
#include "packfile.h"
#include "list-objects-filter-options.h"
#include "odb.h"

int write_filtered_pack(const struct write_pack_opts *opts,
			struct existing_packs *existing,
			struct string_list *names)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct string_list_item *item;
	FILE *in;
	int ret;
	const char *caret;
	const char *pack_prefix = write_pack_opts_pack_prefix(opts);

	prepare_pack_objects(&cmd, opts->po_args, opts->destination);

	strvec_push(&cmd.args, "--stdin-packs");

	for_each_string_list_item(item, &existing->kept_packs)
		strvec_pushf(&cmd.args, "--keep-pack=%s", item->string);

	cmd.in = -1;

	ret = start_command(&cmd);
	if (ret)
		return ret;

	/*
	 * Here 'names' contains only the pack(s) that were just
	 * written, which is exactly the packs we want to keep. Also
	 * 'existing_kept_packs' already contains the packs in
	 * 'keep_pack_list'.
	 */
	in = xfdopen(cmd.in, "w");
	for_each_string_list_item(item, names)
		fprintf(in, "^%s-%s.pack\n", pack_prefix, item->string);
	for_each_string_list_item(item, &existing->non_kept_packs)
		fprintf(in, "%s.pack\n", item->string);
	for_each_string_list_item(item, &existing->cruft_packs)
		fprintf(in, "%s.pack\n", item->string);
	caret = opts->po_args->pack_kept_objects ? "" : "^";
	for_each_string_list_item(item, &existing->kept_packs)
		fprintf(in, "%s%s.pack\n", caret, item->string);
	fclose(in);

	return finish_pack_objects_cmd(existing->repo->hash_algo, opts, &cmd,
				       names);
}

struct enumerate_cb_data {
	struct repository *repo;
	unsigned long min_size;
	int dry_run;
};

static int enumerate_one_object(const struct object_id *oid,
		struct object_info *oi UNUSED,
		void *cb_data)
{
	struct enumerate_cb_data *data = cb_data;
	struct object_info info = OBJECT_INFO_INIT;
	enum object_type type;
	unsigned long size;

	info.typep = &type;
	info.sizep = &size;

	/*
	 * Use OBJECT_INFO_SKIP_FETCH_OBJECT so we never trigger a
	 * lazy fetch while enumerating candidates for removal.
	 */
	if (odb_read_object_info_extended(data->repo->objects, oid, &info,
					  OBJECT_INFO_SKIP_FETCH_OBJECT) < 0)
		return 0;

	if (type != OBJ_BLOB)
		return 0;

	if (data->min_size > 0 && size < data->min_size)
		return 0;

	if (data->dry_run)
		printf("%s\n", oid_to_hex(oid));

	return 0;
}

int enumerate_filtered_objects(struct repository *repo,
			const struct list_objects_filter_options *filter,
			int dry_run)
{
	struct enumerate_cb_data data = { 0 };

	data.repo = repo;
	data.dry_run = dry_run;

	/*
	 * Extract the size threshold from the filter spec.
	 * Only blob:limit=N is supported for now.
	 *
	 * TODO: consider using list_objects_filter__filter_object()
	 * to reuse the existing filter machinery instead of reading
	 * blob_limit_value directly.
	 */

	if (filter->choice == LOFC_BLOB_LIMIT)
		data.min_size = filter->blob_limit_value;

	/*
	 * Walk only promisor objects. Every object visited here is
	 * guaranteed to be recoverable from the promisor remote, so
	 * it is safe to drop without a separate is_promisor_object()
	 * check.
	 *
	 * We do not use write_filtered_pack() here because git repack
	 * routes promisor objects through repack_promisor_objects()
	 * before the filter machinery runs, so the filtered pack never
	 * contains promisor blobs. Direct enumeration via
	 * ODB_FOR_EACH_OBJECT_PROMISOR_ONLY is the only correct approach.
	 */
	return odb_for_each_object(repo->objects, NULL,
			enumerate_one_object, &data,
			ODB_FOR_EACH_OBJECT_PROMISOR_ONLY);
}
