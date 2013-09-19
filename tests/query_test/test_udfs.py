#!/usr/bin/env python
# Copyright (c) 2012 Cloudera, Inc. All rights reserved.

from tests.common.test_vector import *
from tests.common.impala_test_suite import *

class TestUdfs(ImpalaTestSuite):
  @classmethod
  def get_workload(cls):
    return 'functional-query'

  @classmethod
  def add_test_dimensions(cls):
    super(TestUdfs, cls).add_test_dimensions()
    # UDFs require codegen
    cls.TestMatrix.add_constraint(
      lambda v: v.get_value('exec_option')['disable_codegen'] == False)
    # There is no reason to run these tests using all dimensions.
    cls.TestMatrix.add_constraint(lambda v:\
        v.get_value('table_format').file_format == 'text' and\
        v.get_value('table_format').compression_codec == 'none')

  def test_udfs(self, vector):
    self.run_test_case('QueryTest/udf', vector)
