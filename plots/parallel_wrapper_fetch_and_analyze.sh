#!/bin/bash
# Run analyze in provided directory; exit if dir exists

if [ "$#" -lt 1 ]; then
    usage="Provide name of directory for results"
    echo $usage
    exit 1
fi

if [ -d $1 ]; then
    echo "Provided directory exists" 
    exit 1
fi

mkdir $1 
pushd $1

start_time=`date +%s`
# parallel takes space-separated list of args to fetch_and_analyze
args=""

push_args() { 
    cur_date=$1
    end_date=$2
    while [ "$cur_date" != "$end_date" ]; do
        next_date=$(date -I -d "$cur_date + 1 day")
        args=" $args $cur_date:$next_date"
        cur_date=$next_date
    done
}

push_args "2019-01-26" "2019-08-07"
push_args "2019-08-30" "2019-09-12"

echo $args

parallel ~/puffer-statistics/plots/fetch_and_analyze.sh ::: $args

end_time=`date +%s`
runtime=$((end_time-start_time))
echo "wrapper runtime, min: " $(($runtime/60))

popd
