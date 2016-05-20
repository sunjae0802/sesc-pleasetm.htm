#!/bin/bash
#
ARRAYID=$1

. ${SESCDIR}/scripts/setup_stamp.sh
. ${SESCDIR}/scripts/setup_sesc.sh

# Arguments (CONFIG_ID BENCH_ID TID)
CONFIG_ID=$((ARRAYID / 100))
ARRAYID=$((ARRAYID % 100))
BENCH_ID=$((ARRAYID / 10))
benchname=`get_benchname $BENCH_ID`
T_ID=$((ARRAYID % 10))
THREADS=( 1 2 4 8 16 )
nthread=${THREADS[$T_ID]}

get_smp_config $CONFIG_ID
INPUTSIZE="sim"

tmp_name=`mktemp -d`
pushd ${tmp_name}

cp ${SESCDIR}/confs/${SESCCONF} .
cp ${SESCDIR}/confs/${HTMCONF} trans.conf

setup_stamp "$BENCHDIR" "$benchname" "$BENCHCONFIG" "$nthread" "$INPUTSIZE"
run_sesc "${SESCDIR}/build.smp/sesc" "${SESCCONF}" "${PROG}" "${PROGARGS}"

popd
# Reinit benchname
benchname=`get_benchname $BENCH_ID`
benchrundir_name=`get_benchrundir_name "${benchname}-${nthread}-${L1CONF}"`
mv -n ${tmp_name} ${benchrundir_name}
echo "Copied to ${benchrundir_name}"
