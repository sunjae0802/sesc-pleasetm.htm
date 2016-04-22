#!/bin/sh
COMPRESS_CMD="bzip2"
COMPRESS_EXT="bz2"

get_benchrundir() {
    BENCHRUNDIR=$1

    if [ -e "$BENCHRUNDIR" ]; then
        rundir=$BENCHRUNDIR
        var0=0
        while [ -e "$rundir" ]
        do
            rundir="${BENCHRUNDIR}.${var0}"
            var0=`expr $var0 + 1`
        done
        BENCHRUNDIR=$rundir
    fi
}
get_benchrundir_name() {
    BENCHRUNDIR=$1

    if [ -e "$BENCHRUNDIR" ]; then
        rundir=$BENCHRUNDIR
        var0=0
        while [ -e "$rundir" ]
        do
            rundir="${BENCHRUNDIR}.${var0}"
            var0=`expr $var0 + 1`
        done
        BENCHRUNDIR=$rundir
    fi
    echo $BENCHRUNDIR
}

enter_benchrundir() {
    BENCHRUNDIR=$1

    if [ -e "$BENCHRUNDIR" ]; then
        rundir=$BENCHRUNDIR
        var0=0
        while [ -e "$rundir" ]
        do
            rundir="${BENCHRUNDIR}.${var0}"
            var0=`expr $var0 + 1`
        done
        BENCHRUNDIR=$rundir
    fi

    echo "Entering $BENCHRUNDIR ..."
    mkdir -p $BENCHRUNDIR
    cd $BENCHRUNDIR
}

run_sesc() {
    sesccmd=$1
    conf=$2
    bbbin=$3
    bbopt=$4
    out="output.txt"

    cmd="${sesccmd} -c${conf} -w0 ${bbbin} ${bbopt} | tee ${out}"
    echo "Command: $cmd" | tee command.txt
    echo ""

    # Run SESC
    #mkfifo datafile.out
    eval $cmd || exit 42
    #cat datafile.out | ${COMPRESS_CMD} -c > datafile.out.${COMPRESS_EXT}

    # Cleanup
    ${COMPRESS_CMD} datafile.out
    #rm datafile.out

    ${COMPRESS_CMD} sesc_*
    tar -cf src.tar _src
    ${COMPRESS_CMD} src.tar
    rm -rf _src
    rm -rf inputs
}
