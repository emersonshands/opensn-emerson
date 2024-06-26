// SPDX-FileCopyrightText: 2024 The OpenSn Authors <https://open-sn.github.io/opensn/>
// SPDX-License-Identifier: MIT

#pragma once

#include "framework/field_functions/operations/field_operation.h"

namespace opensn
{
class GraphPartitioner;

class PartitionerPredicate : public FieldOperation
{
public:
  static InputParameters GetInputParameters();
  explicit PartitionerPredicate(const InputParameters& params);

  void Execute() override;

protected:
  GraphPartitioner& partitioner_;
  const ParameterBlock result_field_param_;
  const size_t num_partitions_;
  const size_t result_component_;
};

} // namespace opensn
