#!/bin/bash
#
ARRAYID=$1

. ${SESCDIR}/scripts/setup_stamp.sh
. ${SESCDIR}/scripts/setup_sesc.sh

# Arguments (CACHE_ID BENCH_ID TID)
SEED_BASE=0
CACHE_ID=$((ARRAYID / 100))
ARRAYID=$((ARRAYID % 100))
BENCH_ID=$((ARRAYID / 10))
benchname=`get_benchname $BENCH_ID`
T_ID=$((ARRAYID % 10))
THREADS=( 1 2 4 8 16 32 64 128 )
nthread=${THREADS[$T_ID]}

NCORES="144"
l1conf=`get_l1conf $CACHE_ID`
CONF="cmp${NCORES}-${l1conf}.conf"

BENCHCONFIG="gspin"
INPUTSIZE="sim"

tmp_name=`mktemp -d`
pushd ${tmp_name}

cp ${SESCDIR}/confs/${CONF} .
cp ${SESCDIR}/confs/meshAA.booksim booksim.conf
cp ${SESCDIR}/confs/tsx-trans.conf trans.conf

setup_stamp "$BENCHDIR" "$benchname" "$BENCHCONFIG" "$nthread" "$INPUTSIZE" "$SEED_BASE"
run_sesc "${SESCDIR}/build.cmp/sesc" "$CONF" "${PROG}" "${PROGARGS}"

popd
# Reinit benchname
benchname=`get_benchname $BENCH_ID`
benchrundir_name=`get_benchrundir_name "${benchname}-${nthread}-${l1conf}"`
mv -n ${tmp_name} ${benchrundir_name}
echo "Copied to ${benchrundir_name}"
