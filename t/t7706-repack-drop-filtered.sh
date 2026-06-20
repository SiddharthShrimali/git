#!/bin/sh

test_description='git repack --drop-filtered enumerates filtered objects'

. ./test-lib.sh

test_expect_success 'setup' '
	git init repo &&
	(
		cd repo &&
		test-tool genrandom small1 512 >small1.bin &&
		test-tool genrandom small2 512 >small2.bin &&
		test-tool genrandom large1 3072 >large1.bin &&
		test-tool genrandom large2 4096 >large2.bin &&
		git add . &&
		git commit -m "initial commit"
	) &&
	git clone --bare repo bare.git &&
	git -C bare.git repack -a -d
'

test_expect_success '--drop-filtered requires --filter' '
	test_must_fail git -C bare.git repack --drop-filtered -a 2>err &&
	test_grep "drop-filtered requires --filter" err
'

test_expect_success '--drop-filtered cannot be used with --filter-to' '
	test_must_fail git -C bare.git repack --drop-filtered \
		--filter=blob:limit=1k --filter-to=./filter-out 2>err &&
	test_grep "options .--drop-filtered. and .--filter-to. cannot be used together" err
'

test_expect_success '--dry-run only takes effect with --drop-filtered' '
	test_must_fail git -C bare.git repack --dry-run 2>err &&
	test_grep "dry-run only takes effect with --drop-filtered" err
'

test_expect_success '--drop-filtered requires -a' '
	test_must_fail git -C bare.git repack --drop-filtered \
		--filter=blob:limit=1k --dry-run 2>err &&
	test_grep "drop-filtered requires -a" err
'

test_expect_success '--drop-filtered fails with --write-bitmap-index' '
	test_must_fail git -C bare.git repack --drop-filtered \
		--filter=blob:limit=1k --dry-run -a -b 2>err &&
	test_grep "options .--drop-filtered. and .--write-bitmap-index. cannot be used together" err
'

test_expect_success '--dry-run lists large blobs but not small ones' '
	LARGE1=$(git -C repo rev-parse HEAD:large1.bin) &&
	LARGE2=$(git -C repo rev-parse HEAD:large2.bin) &&
	SMALL1=$(git -C repo rev-parse HEAD:small1.bin) &&
	SMALL2=$(git -C repo rev-parse HEAD:small2.bin) &&

	git -C bare.git -c repack.writeBitmaps=false \
		repack --drop-filtered --filter=blob:limit=1k --dry-run -a >out &&

	test_grep "$LARGE1" out &&
	test_grep "$LARGE2" out &&
	test_grep ! "$SMALL1" out &&
	test_grep ! "$SMALL2" out
'

test_expect_success '--dry-run does not remove the filtered objects ' '
	LARGE1=$(git -C repo rev-parse HEAD:large1.bin) &&
	LARGE2=$(git -C repo rev-parse HEAD:large2.bin) &&

	# After --dry-run, the large blobs that would be dropped must
	# still be present in the repository.

	git -C bare.git -c repack.writeBitmaps=false \
		repack --drop-filtered --filter=blob:limit=1k --dry-run -a >out &&

	git -C bare.git cat-file -e "$LARGE1" &&
	git -C bare.git cat-file -e "$LARGE2"
'

test_done
