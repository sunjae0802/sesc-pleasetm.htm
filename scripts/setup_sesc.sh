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

get_smp_config() {
    config_id=$1

    NCORES="16"
    L1CONF="l1_64k_16"
    SESCCONF="smp${NCORES}-${L1CONF}.conf"
    case "$config_id" in
        0)
            BENCHCONFIG="gspin"
            HTMCONF="tsx-trans.conf"
            ;;
        1)
            BENCHCONFIG="htm-spin-lin"
            HTMCONF="tsx-trans.conf"
            ;;
        2)
            BENCHCONFIG="htm-spin-lin"
            HTMCONF="morereads-wins-trans.conf"
            ;;
        3)
            BENCHCONFIG="htm-spin-lin"
            HTMCONF="requester-loses-trans.conf"
            ;;
        *)
            echo "ERROR: Config ${config_id} not found"
            exit 2
            ;;
    esac
}

get_cmp_config() {
    config_id=$1
    NCORES="36"
    L1CONF="l1_64k_16"
    SESCCONF="cmp${NCORES}-${L1CONF}.conf"

    # Handle booksim config since it depends on NCORES
    if [ "$NCORES" -eq 144 ]; then
        BOOKSIMCONF="meshAA.booksim"
    elif [ "$NCORES" -eq 64 ]; then
        BOOKSIMCONF="mesh88.booksim"
    elif [ "$NCORES" -eq 36 ]; then
        BOOKSIMCONF="mesh66.booksim"
    elif [ "$NCORES" -eq 16 ]; then
        BOOKSIMCONF="mesh44.booksim"
    fi

    case "$config_id" in
        0)
            BENCHCONFIG="gspin"
            HTMCONF="tsx-trans.conf"
            ;;
        1)
            BENCHCONFIG="htm-spin-lin"
            HTMCONF="tsx-trans.conf"
            ;;
        2)
            BENCHCONFIG="htm-spin-lin"
            HTMCONF="morereads-wins-trans.conf"
            ;;
        3)
            BENCHCONFIG="htm-spin-lin"
            HTMCONF="requester-loses-trans.conf"
            ;;
        *)
            echo "ERROR: Config ${config_id} not found"
            exit 2
            ;;
    esac
}

run_sesc() {
    sesccmd=$1
    sescconf=$2
    mipsbinary=$3
    binaryopt=$4
    outfile="output.txt"

    cmd="${sesccmd} -c${sescconf} -w0 ${mipsbinary} ${binaryopt} | tee ${outfile}"
    echo "Command: $cmd" | tee command.txt
    echo ""

    # Run SESC
    #mkfifo datafile.out
    eval $cmd || exit 42
    #cat datafile.out | ${COMPRESS_CMD} -c > datafile.out.${COMPRESS_EXT}

    # Cleanup
    echo "Cleaning up"
    if [ -f datafile.out ]; then
        ${COMPRESS_CMD} datafile.out
    fi
    #rm datafile.out

    ${COMPRESS_CMD} sesc_*
    tar -cf src.tar src
    ${COMPRESS_CMD} src.tar
    rm -rf src
    rm -rf inputs
    rm -f ${mipsbinary}
}
