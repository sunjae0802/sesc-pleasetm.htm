#!/bin/sh

setup_stamp() {
    benchdir=$1
    benchname=$2
    benchconfig=$3
    nthread=$4
    inputsize=$5

    tmlib_seed_base=0
    echo "Benchmark $benchname (${benchconfig}; Size ${inputsize}) with $nthread threads"

    case $benchname in 
        "bayes")
            PROG="${benchname}"
            if [ "$inputsize" == "native" ]
            then
                INPF="-v32 -r4096 -n10 -p40 -i2 -e8 -s1"
            elif [ "$inputsize" == "largesparse" ]
            then
                INPF="-v32 -r2048 -n4 -p20 -i2 -e2 -s0"
            elif [ "$inputsize" == "fastm" ]
            then
                INPF="-v32 -r1024 -n2 -p20 -i2 -e2 -s0"
            elif [ "$inputsize" == "sim" ]
            then
                INPF="-v32 -r1024 -n2 -p20 -i2 -e2 -s0"
            fi
            PROGARGS="${INPF} -t${nthread} -S${tmlib_seed_base}"
            ;;
        "genome")
            PROG="${benchname}"
            if [ "$inputsize" == "native" ]
            then
                INPF="-g16384 -s64 -n16777216"
            elif [ "$inputsize" == "largesparse" ]
            then
                INPF="-g4096 -s16 -n65536"
            elif [ "$inputsize" == "fastm" ]
            then
                INPF="-g1024 -s32 -n65536"
            elif [ "$inputsize" == "sim" ]
            then
                INPF="-g256 -s16 -n16384"
            fi
            PROGARGS="${INPF} -t${nthread} -S${tmlib_seed_base}"
            ;;
        "intruder")
            PROG="${benchname}"
            if [ "$inputsize" == "native" ]
            then
                INPF="-a10 -l128 -n262144 -s1"
            elif [ "$inputsize" == "largesparse" ]
            then
                INPF="-a10 -l32 -n8192 -s1"
            elif [ "$inputsize" == "fastm" ]
            then
                INPF="-a10 -l4 -n4096 -s1"
            elif [ "$inputsize" == "sim" ]
            then
                INPF="-a10 -l4 -n2048 -s1"
            fi
            PROGARGS="${INPF} -t${nthread} -S${tmlib_seed_base}"
            ;;
        "kmeans")
            PROG="${benchname}"
            if [ "$inputsize" == "native" ]
            then
                INPF="-m40 -n40 -T0.00001 -i inputs/random-n65536-d32-c16.txt"
            elif [ "$inputsize" == "fastm" ]
            then
                INPF="-m15 -n15 -T0.05 -i inputs/random-n16384-d24-c16.txt"
            elif [ "$inputsize" == "largesparse" ]
            then
                INPF="-m40 -n40 -T0.001 -i inputs/random-n16384-d24-c16.txt"
            elif [ "$inputsize" == "sim" ]
            then
                INPF="-m40 -n40 -T0.05 -i inputs/random-n2048-d16-c16.txt"
            fi
            PROGARGS="${INPF} -t${nthread} -S${tmlib_seed_base}"
            cp -rf ${benchdir}/data/${benchname}/inputs .
            ;;
        "kmeans+")
            PROG="kmeans"
            benchname="kmeans"  # XXX Override benchname
            if [ "$inputsize" == "native" ]
            then
                INPF="-m15 -n15 -T0.00001 -i inputs/random-n65536-d32-c16.txt"
            elif [ "$inputsize" == "m" ]
            then
                INPF="-m15 -n15 -T0.05 -i inputs/random-n16384-d24-c16.txt"
            elif [ "$inputsize" == "largesparse" ]
            then
                INPF="-m15 -n15 -T0.001 -i inputs/random-n16384-d24-c16.txt"
            elif [ "$inputsize" == "sim" ]
            then
                INPF="-m15 -n15 -T0.05 -i inputs/random-n2048-d16-c16.txt"
            fi
            PROGARGS="${INPF} -t${nthread} -S${tmlib_seed_base}"
            cp -rf ${benchdir}/data/${benchname}/inputs .
            ;;
        "labyrinth")
            PROG="${benchname}"
            if [ "$inputsize" == "native" ]
            then
                INPF="-i inputs/random-x512-y512-z7-n512.txt"
            elif [ "$inputsize" == "largesparse" ]
            then
                INPF="-i inputs/random-x48-y48-z3-n64.txt"
            elif [ "$inputsize" == "fastm" ]
            then
                INPF="-i inputs/random-x32-y32-z3-n1024.txt"
            elif [ "$inputsize" == "sim" ]
            then
                INPF="-i inputs/random-x32-y32-z3-n96.txt"
            fi
            PROGARGS="${INPF} -t${nthread} -S${tmlib_seed_base}"
            cp -rf ${benchdir}/data/${benchname}/inputs .
            ;;
        "ssca2")
            PROG="${benchname}"
            if [ "$inputsize" == "native" ]
            then
                INPF="-s20 -i1.0 -u1.0 -l3 -p3"
            elif [ "$inputsize" == "m" ]
            then
                INPF="-s14 -i1.0 -u1.0 -l9 -p9"
            elif [ "$inputsize" == "fastm" ]
            then
                INPF="-s14 -i1.0 -u1.0 -l9 -p9"
            elif [ "$inputsize" == "largesparse" ]
            then
                INPF="-s14 -i1.0 -u1.0 -l9 -p9"
            elif [ "$inputsize" == "sim" ]
            then
                INPF="-s13 -i1.0 -u1.0 -l3 -p3"
            fi
            PROGARGS="${INPF} -t${nthread} -S${tmlib_seed_base}"
            ;;
        "vacation")
            PROG="${benchname}"
            if [ "$inputsize" == "native" ]
            then
                INPF="-n2 -q90 -u98 -r1048576 -T4194304"
            elif [ "$inputsize" == "fastm" ]
            then
                INPF="-n4 -q60 -u90 -r65536 -T4096"
            elif [ "$inputsize" == "largesparse" ]
            then
                INPF="-n2 -q90 -u98 -r65536 -T16384"
            elif [ "$inputsize" == "sim" ]
            then
                INPF="-n2 -q90 -u98 -r16384 -T4096"
            fi
            PROGARGS="${INPF} -t${nthread} -S${tmlib_seed_base}"
            ;;
        "vacation+")
            PROG="vacation"
            benchname="vacation"  # XXX Override benchname
            if [ "$inputsize" == "native" ]
            then
                INPF="-n4 -q60 -u90 -r1048576 -T4194304"
            elif [ "$inputsize" == "m" ]
            then
                INPF="-n4 -q60 -u90 -r65536 -T4096"
            elif [ "$inputsize" == "largesparse" ]
            then
                INPF="-n4 -q60 -u90 -r65536 -T16384"
            elif [ "$inputsize" == "sim" ]
            then
                INPF="-n4 -q60 -u90 -r16384 -T4096"
            fi
            PROGARGS="${INPF} -t${nthread} -S${tmlib_seed_base}"
            ;;
        "yada")
            PROG="${benchname}"
            if [ "$inputsize" == "native" ]
            then
                INPF="-a15 -i inputs/ttimeu1000000.2"
            elif [ "$inputsize" == "m" ]
            then
                INPF="-a20 -i inputs/ttimeu100000.2"
            elif [ "$inputsize" == "largesparse" ]
            then
                INPF="-a20 -i inputs/ttimeu100000.2"
            elif [ "$inputsize" == "fastm" ]
            then
                INPF="-a20 -i inputs/633.2"
            elif [ "$inputsize" == "sim" ]
            then
                INPF="-a20 -i inputs/633.2"
            fi
            PROGARGS="${INPF} -t${nthread} -S${tmlib_seed_base}"
            cp -rf ${benchdir}/data/${benchname}/inputs .
            ;;
        *)
            echo "ERROR: Benchmark ${benchname} not found"
            exit 2
        ;;
    esac
    if [ -z "$INPF" ]
    then
        echo "ERROR: Input size ${inputsize} not found"
        exit 3
    fi

    # Copy source and objects
    cp -rf "${benchdir}/${benchconfig}/${benchname}/${PROG}" .
    mkdir src

    cp -rf ${benchdir}/trunk/${benchname} src/
    cp -rf ${benchdir}/trunk/lib src/
    cp -rf ${benchdir}/trunk/common/${benchconfig}/ src/
}

get_benchname() {
    benchid=$1
    #            0     10     20       30     40      50        60    70       80        90
    BENCHMARKS=( bayes genome intruder kmeans kmeans+ labyrinth ssca2 vacation vacation+ yada )
    echo ${BENCHMARKS[$benchid]}
}

