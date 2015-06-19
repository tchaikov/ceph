#!/bin/bash
#
# Copyright (C) 2015 Red Hat <contact@redhat.com>
#
# Author: Kefu Chai <kchai@redhat.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Library Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Library Public License for more details.
#

verbose=

test -d ../src && export PATH=$PATH:.

function osdmap_get() {
    local store_path=$1
    local query=$2
    local epoch=${3:+-v $3}
    local osdmap=`mktemp`

    ceph-monstore-tool $store_path get osdmap -- \
                           $epoch -o $osdmap > /dev/null
    echo $(osdmaptool --dump-json $osdmap 2> /dev/null | jq "$query")

    rm -f $osdmap
}

function test_crush() {
    local store_path=$1
    local epoch=$2
    local max_osd=$3
    local crush=$4
    local osdmap=`mktemp`

    ceph-monstore-tool $store_path get osdmap -- \
                       -v $epoch -o $osdmap > /dev/null
    osdmaptool --export-crush $crush $osdmap &> /dev/null

    if crushtool --check $max_osd -i $crush > /dev/null; then
        good=true
    else
        good=false
    fi
    rm -f $osdmap
    $good || return 1
}

function get_crush()  {
    local store_path=$1
    local osdmap_epoch=$2
    local osdmap_path=`mktemp`
    local crush_path=`mktemp`

    ceph-monstore-tool $store_path get osdmap -- \
                       -v $osdmap_epoch -o $osdmap_path
    osdmaptool --export-crush $crush $osdmap_path 2>&1 > /dev/null
}

function usage() {
    cat <<EOF
look for a latest known-good crush map in history and rewrite the monitor
storge with it.
$0 [options] ...
  [--verbose]            be more chatty
  [-h|--help]            display this message
  [--rewrite]            rewrite the monitor storage with the found crush map
  [--mon-store]          monitor storage path
  [--out]                put the found crush map into given file
  [--osdmap-epoch]       instead using the latest committed osdmap, use the
                         given one
EOF
}

function main() {
    local temp
    temp=$(getopt -o h --long verbose,help,mon-store:,osdmap-epoch:,out:,rewrite -n $0 -- "$@") || return 1

    eval set -- "$temp"
    local rewrite
    while true; do
        case "$1" in
            --verbose)
                verbose=true
                # set -xe
                # PS4='${FUNCNAME[0]}: $LINENO: '
                shift;;
            -h|--help)
                usage
                return 0;;
            --mon-store)
                store_path=$2
                shift 2;;
            --out)
                output=$2
                shift 2;;
            --osdmap-epoch)
                osdmap_epoch=$2
                shift 2;;
            --rewrite)
                rewrite=true
                shift;;
            --)
                shift
                break;;
            *)
                echo "unexpected argument $1"
                usage
                return 1;;
        esac
    done

    if ! test $store_path; then
        usage
        return 0
    fi

    local last_osdmap_epoch=$(osdmap_get $store_path ".epoch")
    osdmap_epoch=${osdmap_epoch:-$last_osdmap_epoch}
    max_osd=$(osdmap_get $store_path ".max_osd" $osdmap_epoch)

    local good_crush
    local good_epoch
    test $verbose && echo "the latest osdmap epoch is $last_osdmap_epoch"
    for epoch in `seq $last_osdmap_epoch -1 1`; do
        local crush_path=`mktemp`
        test $verbose && echo "checking crush map #$epoch"
        if test_crush $store_path $epoch $max_osd $crush_path; then
            test $verbose && echo "crush map version #$epoch works with osdmap epoch #$osdmap_epoch"
            good_epoch=$epoch
            good_crush=$crush_path
            break
        fi
        rm -f $crush_path
    done

    if test $good_epoch; then
        echo "good crush map found at epoch $epoch/$last_osdmap_epoch"
    else
        echo "Unable to find a crush map for osdmap version #$osdmap_epoch." 2>&1
        return 1
    fi

    if test $good_epoch -eq $last_osdmap_epoch; then
        echo "and mon store has no faulty crush maps."
    elif test $output; then
        crushtool --decompile $good_crush --outfn $output
    elif test $rewrite; then
        ceph-monstore-tool $store_path rewrite-crush --  \
                           --crush-path $good_crush      \
                           --osdmap-version $good_epoch
    else
        echo
        crushtool --decompile $good_crush
    fi
    rm -f $good_crush
}

main "$@"
