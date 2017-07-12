#!/bin/sh
#
# Copyright (c) 2016 Huawei Technologies Co., Ltd. All rights reserved.
#

test_description='0002 test ()

    This test start sg processes: sg_client, sg_server and tgt service.'

# include sharness lib
. ./lib/sharness/sharness.sh

# get curret work path
AUTO_HOME=${AUTO_HOME:-$(cd $(dirname $0)/..;pwd)}

# set sg env
#source ${AUTO_HOME}/sg_env.sh

test_expect_success 'start sg services' '
    cd ${AUTO_HOME} &&
    ./sg_test.sh start &&
    sleep 10
'

test_done
