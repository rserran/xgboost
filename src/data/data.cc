/**
 * Copyright 2015-2025, XGBoost Contributors
 * \file data.cc
 */
#include "xgboost/data.h"

#include <dmlc/registry.h>  // for DMLC_REGISTRY_ENABLE, DMLC_REGISTRY_LINK_TAG

#include <algorithm>    // for copy, max, none_of, min
#include <atomic>       // for atomic
#include <cmath>        // for abs
#include <cstdint>      // for uint64_t, int32_t, uint8_t, uint32_t
#include <cstring>      // for size_t, strcmp, memcpy
#include <iostream>     // for operator<<, basic_ostream, basic_ostream::op...
#include <map>          // for map, operator!=
#include <numeric>      // for accumulate, partial_sum
#include <tuple>        // for get, apply
#include <type_traits>  // for remove_pointer_t, remove_reference

#include "../collective/allgather.h"          // for AllgatherStrings
#include "../collective/allreduce.h"          // for Allreduce
#include "../collective/communicator-inl.h"   // for GetRank, IsFederated
#include "../common/algorithm.h"              // for StableSort
#include "../common/api_entry.h"              // for XGBAPIThreadLocalEntry
#include "../common/error_msg.h"              // for GroupSize, GroupWeight, InfInData
#include "../common/group_data.h"             // for ParallelGroupBuilder
#include "../common/io.h"                     // for PeekableInStream
#include "../common/linalg_op.h"              // for ElementWiseTransformHost
#include "../common/math.h"                   // for CheckNAN
#include "../common/numeric.h"                // for Iota, RunLengthEncode
#include "../common/threading_utils.h"        // for ParallelFor
#include "../common/version.h"                // for Version
#include "../data/adapter.h"                  // for COOTuple, FileAdapter, IsValidFunctor
#include "../data/extmem_quantile_dmatrix.h"  // for ExtMemQuantileDMatrix
#include "../data/iterative_dmatrix.h"        // for IterativeDMatrix
#include "./sparse_page_dmatrix.h"            // for SparsePageDMatrix
#include "array_interface.h"                  // for ArrayInterfaceHandler, ArrayInterface, Dispa...
#include "cat_container.h"                    // for CatContainer
#include "dmlc/base.h"                        // for BeginPtr
#include "dmlc/common.h"                      // for OMPException
#include "dmlc/data.h"                        // for Parser
#include "dmlc/endian.h"                      // for ByteSwap, DMLC_IO_NO_ENDIAN_SWAP
#include "dmlc/io.h"                          // for Stream
#include "dmlc/thread_local.h"                // for ThreadLocalStore
#include "ellpack_page.h"                     // for EllpackPage
#include "file_iterator.h"                    // for ValidateFileFormat, FileIterator, Next, Reset
#include "gradient_index.h"                   // for GHistIndexMatrix
#include "simple_dmatrix.h"                   // for SimpleDMatrix
#include "sparse_page_writer.h"               // for SparsePageFormatReg
#include "validation.h"                       // for LabelsCheck, WeightsCheck, ValidateQueryGroup
#include "xgboost/base.h"                     // for bst_group_t, bst_idx_t, bst_float, bst_ulong
#include "xgboost/context.h"                  // for Context
#include "xgboost/host_device_vector.h"       // for HostDeviceVector
#include "xgboost/learner.h"                  // for HostDeviceVector
#include "xgboost/linalg.h"                   // for Tensor, Stack, TensorView, Vector, ArrayInte...
#include "xgboost/logging.h"                  // for Error, LogCheck_EQ, CHECK, CHECK_EQ, LOG
#include "xgboost/span.h"                     // for Span, operator!=, SpanIterator
#include "xgboost/string_view.h"              // for operator==, operator<<, StringView

namespace dmlc {
DMLC_REGISTRY_ENABLE(::xgboost::data::SparsePageFormatReg<::xgboost::SparsePage>);
DMLC_REGISTRY_ENABLE(::xgboost::data::SparsePageFormatReg<::xgboost::CSCPage>);
DMLC_REGISTRY_ENABLE(::xgboost::data::SparsePageFormatReg<::xgboost::SortedCSCPage>);
DMLC_REGISTRY_ENABLE(::xgboost::data::SparsePageFormatReg<::xgboost::EllpackPage>);
DMLC_REGISTRY_ENABLE(::xgboost::data::SparsePageFormatReg<::xgboost::GHistIndexMatrix>);
}  // namespace dmlc

namespace {

template <typename T>
void SaveScalarField(dmlc::Stream *strm, const std::string &name,
                     xgboost::DataType type, const T &field) {
  strm->Write(name);
  strm->Write(static_cast<uint8_t>(type));
  strm->Write(true);  // is_scalar=True
  strm->Write(field);
}

template <typename T>
void SaveVectorField(dmlc::Stream *strm, const std::string &name,
                     xgboost::DataType type, std::pair<uint64_t, uint64_t> shape,
                     const std::vector<T>& field) {
  strm->Write(name);
  strm->Write(static_cast<uint8_t>(type));
  strm->Write(false);  // is_scalar=False
  strm->Write(shape.first);
  strm->Write(shape.second);
  strm->Write(field);
}

template <typename T>
void SaveVectorField(dmlc::Stream* strm, const std::string& name,
                     xgboost::DataType type, std::pair<uint64_t, uint64_t> shape,
                     const xgboost::HostDeviceVector<T>& field) {
  SaveVectorField(strm, name, type, shape, field.ConstHostVector());
}

template <typename T, int32_t D>
void SaveTensorField(dmlc::Stream* strm, const std::string& name, xgboost::DataType type,
                     const xgboost::linalg::Tensor<T, D>& field) {
  strm->Write(name);
  strm->Write(static_cast<uint8_t>(type));
  strm->Write(false);  // is_scalar=False
  for (size_t i = 0; i < D; ++i) {
    strm->Write(field.Shape(i));
  }
  strm->Write(field.Data()->HostVector());
}

template <typename T>
void LoadScalarField(dmlc::Stream* strm, const std::string& expected_name,
                     xgboost::DataType expected_type, T* field) {
  const std::string invalid{"MetaInfo: Invalid format for " + expected_name};
  std::string name;
  xgboost::DataType type;
  bool is_scalar;
  CHECK(strm->Read(&name)) << invalid;
  CHECK_EQ(name, expected_name)
      << invalid << " Expected field: " << expected_name << ", got: " << name;
  uint8_t type_val;
  CHECK(strm->Read(&type_val)) << invalid;
  type = static_cast<xgboost::DataType>(type_val);
  CHECK(type == expected_type)
      << invalid << "Expected field of type: " << static_cast<int>(expected_type) << ", "
      << "got field type: " << static_cast<int>(type);
  CHECK(strm->Read(&is_scalar)) << invalid;
  CHECK(is_scalar)
    << invalid << "Expected field " << expected_name << " to be a scalar; got a vector";
  CHECK(strm->Read(field)) << invalid;
}

template <typename T>
void LoadVectorField(dmlc::Stream* strm, const std::string& expected_name,
                     xgboost::DataType expected_type, std::vector<T>* field) {
  const std::string invalid{"MetaInfo: Invalid format for " + expected_name};
  std::string name;
  xgboost::DataType type;
  bool is_scalar;
  CHECK(strm->Read(&name)) << invalid;
  CHECK_EQ(name, expected_name)
    << invalid << " Expected field: " << expected_name << ", got: " << name;
  uint8_t type_val;
  CHECK(strm->Read(&type_val)) << invalid;
  type = static_cast<xgboost::DataType>(type_val);
  CHECK(type == expected_type)
    << invalid << "Expected field of type: " << static_cast<int>(expected_type) << ", "
    << "got field type: " << static_cast<int>(type);
  CHECK(strm->Read(&is_scalar)) << invalid;
  CHECK(!is_scalar)
    << invalid << "Expected field " << expected_name << " to be a vector; got a scalar";
  std::pair<uint64_t, uint64_t> shape;

  CHECK(strm->Read(&shape.first));
  CHECK(strm->Read(&shape.second));
  // TODO(hcho3): this restriction may be lifted, once we add a field with more than 1 column.
  CHECK_EQ(shape.second, 1) << invalid << "Number of columns is expected to be 1.";

  CHECK(strm->Read(field)) << invalid;
}

template <typename T>
void LoadVectorField(dmlc::Stream* strm, const std::string& expected_name,
                     xgboost::DataType expected_type,
                     xgboost::HostDeviceVector<T>* field) {
  LoadVectorField(strm, expected_name, expected_type, &field->HostVector());
}

template <typename T, int32_t D>
void LoadTensorField(dmlc::Stream* strm, std::string const& expected_name,
                     xgboost::DataType expected_type, xgboost::linalg::Tensor<T, D>* p_out) {
  const std::string invalid{"MetaInfo: Invalid format for " + expected_name};
  std::string name;
  xgboost::DataType type;
  bool is_scalar;
  CHECK(strm->Read(&name)) << invalid;
  CHECK_EQ(name, expected_name) << invalid << " Expected field: " << expected_name
                                << ", got: " << name;
  uint8_t type_val;
  CHECK(strm->Read(&type_val)) << invalid;
  type = static_cast<xgboost::DataType>(type_val);
  CHECK(type == expected_type) << invalid
                               << "Expected field of type: " << static_cast<int>(expected_type)
                               << ", "
                               << "got field type: " << static_cast<int>(type);
  CHECK(strm->Read(&is_scalar)) << invalid;
  CHECK(!is_scalar) << invalid << "Expected field " << expected_name
                    << " to be a tensor; got a scalar";
  size_t shape[D];
  for (size_t i = 0; i < D; ++i) {
    CHECK(strm->Read(&(shape[i])));
  }
  p_out->Reshape(shape);
  auto& field = p_out->Data()->HostVector();
  CHECK(strm->Read(&field)) << invalid;
}
}  // anonymous namespace

namespace xgboost {

uint64_t constexpr MetaInfo::kNumField;

MetaInfo::MetaInfo() : cats_{std::make_shared<CatContainer>()} {}

// implementation of inline functions
void MetaInfo::Clear() {
  num_row_ = num_col_ = num_nonzero_ = 0;
  labels = decltype(labels){};
  group_ptr_.clear();
  weights_.HostVector().clear();
  base_margin_ = decltype(base_margin_){};
}

/*
 * Binary serialization format for MetaInfo:
 *
 * | name               | type     | is_scalar | num_row     |     num_col | value                  |
 * |--------------------+----------+-----------+-------------+-------------+------------------------|
 * | num_row            | kUInt64  | True      | NA          |          NA | ${num_row_}            |
 * | num_col            | kUInt64  | True      | NA          |          NA | ${num_col_}            |
 * | num_nonzero        | kUInt64  | True      | NA          |          NA | ${num_nonzero_}        |
 * | labels             | kFloat32 | False     | ${size}     |           1 | ${labels_}             |
 * | group_ptr          | kUInt32  | False     | ${size}     |           1 | ${group_ptr_}          |
 * | weights            | kFloat32 | False     | ${size}     |           1 | ${weights_}            |
 * | base_margin        | kFloat32 | False     | ${Shape(0)} | ${Shape(1)} | ${base_margin_}        |
 * | labels_lower_bound | kFloat32 | False     | ${size}     |           1 | ${labels_lower_bound_} |
 * | labels_upper_bound | kFloat32 | False     | ${size}     |           1 | ${labels_upper_bound_} |
 * | feature_names      | kStr     | False     | ${size}     |           1 | ${feature_names}       |
 * | feature_types      | kStr     | False     | ${size}     |           1 | ${feature_types}       |
 * | feature_weights    | kFloat32 | False     | ${size}     |           1 | ${feature_weights}     |
 * | cats               | kStr     | False     | ${size}     |           1 | ${cats}     |
 *
 * Note that the scalar fields (is_scalar=True) will have num_row and num_col missing.
 * Also notice the difference between the saved name and the name used in `SetInfo':
 * the former uses the plural form.
 */

void MetaInfo::SaveBinary(dmlc::Stream *fo) const {
  Version::Save(fo);
  fo->Write(kNumField);
  int field_cnt = 0;  // make sure we are actually writing kNumField fields

  SaveScalarField(fo, u8"num_row", DataType::kUInt64, num_row_); ++field_cnt;
  SaveScalarField(fo, u8"num_col", DataType::kUInt64, num_col_); ++field_cnt;
  SaveScalarField(fo, u8"num_nonzero", DataType::kUInt64, num_nonzero_); ++field_cnt;
  SaveTensorField(fo, u8"labels", DataType::kFloat32, labels); ++field_cnt;
  SaveVectorField(fo, u8"group_ptr", DataType::kUInt32,
                  {group_ptr_.size(), 1}, group_ptr_); ++field_cnt;
  SaveVectorField(fo, u8"weights", DataType::kFloat32,
                  {weights_.Size(), 1}, weights_); ++field_cnt;
  SaveTensorField(fo, u8"base_margin", DataType::kFloat32, base_margin_); ++field_cnt;
  SaveVectorField(fo, u8"labels_lower_bound", DataType::kFloat32,
                  {labels_lower_bound_.Size(), 1}, labels_lower_bound_); ++field_cnt;
  SaveVectorField(fo, u8"labels_upper_bound", DataType::kFloat32,
                  {labels_upper_bound_.Size(), 1}, labels_upper_bound_); ++field_cnt;

  SaveVectorField(fo, u8"feature_names", DataType::kStr, {feature_names.size(), 1}, feature_names);
  ++field_cnt;
  SaveVectorField(fo, u8"feature_types", DataType::kStr, {feature_type_names.size(), 1},
                  feature_type_names);
  ++field_cnt;
  SaveVectorField(fo, u8"feature_weights", DataType::kFloat32, {feature_weights.Size(), 1},
                  feature_weights);
  ++field_cnt;

  Json jcats{Object{}};
  this->cats_->Save(&jcats);
  std::vector<char> values;
  Json::Dump(jcats, &values, std::ios::binary);
  SaveVectorField(fo, u8"cats", DataType::kStr, {values.size(), 1}, values);
  ++field_cnt;

  CHECK_EQ(field_cnt, kNumField) << "Wrong number of fields";
}

/**
 * @brief Load feature type info from names, returns whether there's categorical features.
 */
[[nodiscard]] bool LoadFeatureType(std::vector<std::string> const& type_names,
                                   std::vector<FeatureType>* types) {
  types->clear();
  bool has_cat{false};
  for (auto const& elem : type_names) {
    if (elem == "int") {
      types->emplace_back(FeatureType::kNumerical);
    } else if (elem == "float") {
      types->emplace_back(FeatureType::kNumerical);
    } else if (elem == "i") {
      types->emplace_back(FeatureType::kNumerical);
    } else if (elem == "q") {
      types->emplace_back(FeatureType::kNumerical);
    } else if (elem == "c") {
      types->emplace_back(FeatureType::kCategorical);
      has_cat = true;
    } else {
      LOG(FATAL) << "All feature_types must be one of {int, float, i, q, c}.";
    }
  }
  return has_cat;
}

const std::vector<size_t>& MetaInfo::LabelAbsSort(Context const* ctx) const {
  if (label_order_cache_.size() == labels.Size()) {
    return label_order_cache_;
  }
  label_order_cache_.resize(labels.Size());
  common::Iota(ctx, label_order_cache_.begin(), label_order_cache_.end(), 0);
  const auto& l = labels.Data()->HostVector();
  common::StableSort(ctx, label_order_cache_.begin(), label_order_cache_.end(),
                     [&l](size_t i1, size_t i2) { return std::abs(l[i1]) < std::abs(l[i2]); });

  return label_order_cache_;
}

void MetaInfo::LoadBinary(dmlc::Stream *fi) {
  auto version = Version::Load(fi);
  auto major = std::get<0>(version);
  auto minor = std::get<1>(version);
  // MetaInfo is saved in `SparsePageSource'.  So the version in MetaInfo represents the
  // version of DMatrix.
  std::stringstream msg;
  msg << "Binary DMatrix generated by XGBoost: " << Version::String(version)
      << " is no longer supported. "
      << "Please process and save your data in current version: "
      << Version::String(Version::Self()) << " again.";
  CHECK_GE(major, 3) << msg.str();
  CHECK_GE(minor, 1) << msg.str();

  const uint64_t expected_num_field = kNumField;
  uint64_t num_field { 0 };
  CHECK(fi->Read(&num_field)) << "MetaInfo: invalid format";
  size_t expected = 0;
  if (major == 1 && std::get<1>(version) < 2) {
    // feature names and types are added in 1.2
    expected = expected_num_field - 2;
  } else {
    expected = expected_num_field;
  }
  CHECK_GE(num_field, expected)
      << "MetaInfo: insufficient number of fields (expected at least "
      << expected << " fields, but the binary file only contains " << num_field
      << "fields.)";
  if (num_field > expected_num_field) {
    LOG(WARNING) << "MetaInfo: the given binary file contains extra fields "
                    "which will be ignored.";
  }

  LoadScalarField(fi, u8"num_row", DataType::kUInt64, &num_row_);
  LoadScalarField(fi, u8"num_col", DataType::kUInt64, &num_col_);
  LoadScalarField(fi, u8"num_nonzero", DataType::kUInt64, &num_nonzero_);
  LoadTensorField(fi, u8"labels", DataType::kFloat32, &labels);
  LoadVectorField(fi, u8"group_ptr", DataType::kUInt32, &group_ptr_);
  LoadVectorField(fi, u8"weights", DataType::kFloat32, &weights_);
  LoadTensorField(fi, u8"base_margin", DataType::kFloat32, &base_margin_);
  LoadVectorField(fi, u8"labels_lower_bound", DataType::kFloat32, &labels_lower_bound_);
  LoadVectorField(fi, u8"labels_upper_bound", DataType::kFloat32, &labels_upper_bound_);

  LoadVectorField(fi, u8"feature_names", DataType::kStr, &feature_names);
  LoadVectorField(fi, u8"feature_types", DataType::kStr, &feature_type_names);
  LoadVectorField(fi, u8"feature_weights", DataType::kFloat32, &feature_weights);

  this->has_categorical_ = LoadFeatureType(feature_type_names, &feature_types.HostVector());

  std::vector<char> values;
  LoadVectorField(fi, u8"cats", DataType::kStr, &values);
  auto jcats = Json::Load(StringView{values.data(), values.size()}, std::ios::binary);
  this->cats_->Load(jcats);
}

namespace {
template <typename T>
std::vector<T> Gather(const std::vector<T>& in, common::Span<bst_idx_t const> ridxs,
                      size_t stride = 1) {
  if (in.empty()) {
    return {};
  }
  auto size = ridxs.size();
  std::vector<T> out(size * stride);
  for (auto i = 0ull; i < size; i++) {
    auto ridx = ridxs[i];
    for (size_t j = 0; j < stride; ++j) {
      out[i * stride + j] = in[ridx * stride + j];
    }
  }
  return out;
}
}  // namespace

namespace cuda_impl {
void SliceMetaInfo(Context const* ctx, MetaInfo const& info, common::Span<bst_idx_t const> ridx,
                   MetaInfo* p_out);
#if !defined(XGBOOST_USE_CUDA)
void SliceMetaInfo(Context const*, MetaInfo const&, common::Span<bst_idx_t const>, MetaInfo*) {
  common::AssertGPUSupport();
}
#endif
}  // namespace cuda_impl

MetaInfo MetaInfo::Slice(Context const* ctx, common::Span<bst_idx_t const> ridxs,
                         bst_idx_t nnz) const {
  /**
   * Shape
   */
  MetaInfo out;
  out.num_row_ = ridxs.size();
  out.num_col_ = this->num_col_;
  out.num_nonzero_ = nnz;

  /**
   * Feature Info
   */
  out.feature_weights.SetDevice(ctx->Device());
  out.feature_weights.Resize(this->feature_weights.Size());
  out.feature_weights.Copy(this->feature_weights);

  out.feature_names = this->feature_names;

  out.feature_types.SetDevice(ctx->Device());
  out.feature_types.Resize(this->feature_types.Size());
  out.feature_types.Copy(this->feature_types);

  out.feature_type_names = this->feature_type_names;

  /**
   * Sample Info
   */
  if (ctx->IsCUDA()) {
    cuda_impl::SliceMetaInfo(ctx, *this, ridxs, &out);
    return out;
  }

  // Groups is maintained by a higher level Python function.  We should aim at deprecating
  // the slice function.
  if (this->labels.Size() != this->num_row_) {
    auto t_labels = this->labels.View(this->labels.Data()->Device());
    out.labels.Reshape(ridxs.size(), labels.Shape(1));
    out.labels.Data()->HostVector() =
        Gather(this->labels.Data()->HostVector(), ridxs, t_labels.Stride(0));
  } else {
    out.labels.ModifyInplace([&](auto* data, common::Span<size_t, 2> shape) {
      data->HostVector() = Gather(this->labels.Data()->HostVector(), ridxs);
      shape[0] = data->Size();
      shape[1] = 1;
    });
  }

  out.labels_upper_bound_.HostVector() = Gather(this->labels_upper_bound_.HostVector(), ridxs);
  out.labels_lower_bound_.HostVector() = Gather(this->labels_lower_bound_.HostVector(), ridxs);
  // weights
  if (this->weights_.Size() + 1 == this->group_ptr_.size()) {
    auto& h_weights = out.weights_.HostVector();
    // Assuming all groups are available.
    out.weights_.HostVector() = h_weights;
  } else {
    out.weights_.HostVector() = Gather(this->weights_.HostVector(), ridxs);
  }

  if (this->base_margin_.Size() != this->num_row_) {
    CHECK_EQ(this->base_margin_.Size() % this->num_row_, 0)
        << "Incorrect size of base margin vector.";
    auto t_margin = this->base_margin_.View(this->base_margin_.Data()->Device());
    out.base_margin_.Reshape(ridxs.size(), t_margin.Shape(1));
    out.base_margin_.Data()->HostVector() =
        Gather(this->base_margin_.Data()->HostVector(), ridxs, t_margin.Stride(0));
  } else {
    out.base_margin_.ModifyInplace([&](auto* data, common::Span<size_t, 2> shape) {
      data->HostVector() = Gather(this->base_margin_.Data()->HostVector(), ridxs);
      shape[0] = data->Size();
      shape[1] = 1;
    });
  }

  return out;
}

MetaInfo MetaInfo::Copy() const {
  MetaInfo out;
  out.Extend(*this, /*accumulate_rows=*/true, /*check_column=*/false);
  return out;
}

namespace {
template <int32_t D, typename T>
void CopyTensorInfoImpl(Context const* ctx, Json arr_interface, linalg::Tensor<T, D>* p_out) {
  ArrayInterface<D> array{arr_interface};
  if (array.n == 0) {
    p_out->Reshape(array.shape);
    return;
  }
  CHECK_EQ(array.valid.Capacity(), 0)
      << "Meta info like label or weight can not have missing value.";
  if (array.is_contiguous && array.type == ToDType<T>::kType) {
    // Handle contigious
    p_out->ModifyInplace([&](HostDeviceVector<T>* data, common::Span<size_t, D> shape) {
      // set shape
      std::copy(array.shape, array.shape + D, shape.data());
      // set data
      data->Resize(array.n);
      std::memcpy(data->HostPointer(), array.data, array.n * sizeof(T));
    });
    return;
  }
  p_out->Reshape(array.shape);
  auto t_out = p_out->View(DeviceOrd::CPU());
  CHECK(t_out.CContiguous());
  auto const shape = t_out.Shape();
  DispatchDType(array, DeviceOrd::CPU(), [&](auto&& in) {
    linalg::ElementWiseTransformHost(t_out, ctx->Threads(), [&](auto i, auto) {
      return std::apply(in, linalg::UnravelIndex<D>(i, shape));
    });
  });
}
}  // namespace

void MetaInfo::SetInfo(Context const& ctx, StringView key, StringView interface_str) {
  Json j_interface = Json::Load(interface_str);
  bool is_cuda{false};
  if (IsA<Array>(j_interface)) {
    auto const& array = get<Array const>(j_interface);
    CHECK_GE(array.size(), 0) << "Invalid " << key
                              << ", must have at least 1 column even if it's empty.";
    auto const& first = get<Object const>(array.front());
    auto ptr = ArrayInterfaceHandler::GetPtrFromArrayData<void*>(first);
    is_cuda = first.find("stream") != first.cend() || ArrayInterfaceHandler::IsCudaPtr(ptr);
  } else {
    auto const& first = get<Object const>(j_interface);
    auto ptr = ArrayInterfaceHandler::GetPtrFromArrayData<void*>(first);
    is_cuda = first.find("stream") != first.cend() || ArrayInterfaceHandler::IsCudaPtr(ptr);
  }

  if (is_cuda) {
    this->SetInfoFromCUDA(&ctx, key, j_interface);
  } else {
    this->SetInfoFromHost(&ctx, key, j_interface);
  }
}

void MetaInfo::SetInfoFromHost(Context const* ctx, StringView key, Json arr) {
  // multi-dim float info
  if (key == "base_margin") {
    CopyTensorInfoImpl(ctx, arr, &this->base_margin_);
    // FIXME(jiamingy): Remove the deprecated API and let all language bindings aware of
    // input shape.  This issue is CPU only since CUDA uses array interface from day 1.
    //
    // Python binding always understand the shape, so this condition should not occur for
    // it.
    if (this->num_row_ != 0 && this->base_margin_.Shape(0) != this->num_row_) {
      // API functions that don't use array interface don't understand shape.
      CHECK(this->base_margin_.Size() % this->num_row_ == 0) << "Incorrect size for base margin.";
      size_t n_groups = this->base_margin_.Size() / this->num_row_;
      this->base_margin_.Reshape(this->num_row_, n_groups);
    }
    return;
  } else if (key == "label") {
    CopyTensorInfoImpl(ctx, arr, &this->labels);
    if (this->num_row_ != 0 && this->labels.Shape(0) != this->num_row_) {
      CHECK_EQ(this->labels.Size() % this->num_row_, 0)
          << "Incorrect size for labels: (" << this->labels.Shape(0) << "," << this->labels.Shape(1)
          << ") v.s. " << this->num_row_;
      size_t n_targets = this->labels.Size() / this->num_row_;
      this->labels.Reshape(this->num_row_, n_targets);
    }
    auto const& h_labels = labels.Data()->ConstHostVector();
    auto valid = std::none_of(h_labels.cbegin(), h_labels.cend(), data::LabelsCheck{});
    CHECK(valid) << "Label contains NaN, infinity or a value too large.";
    return;
  }
  // uint info
  if (key == "group") {
    linalg::Vector<bst_group_t> t;
    CopyTensorInfoImpl(ctx, arr, &t);
    auto const& h_groups = t.Data()->HostVector();
    group_ptr_.clear();
    group_ptr_.resize(h_groups.size() + 1, 0);
    group_ptr_[0] = 0;
    std::partial_sum(h_groups.cbegin(), h_groups.cend(), group_ptr_.begin() + 1);
    data::ValidateQueryGroup(group_ptr_);
    return;
  } else if (key == "qid") {
    linalg::Tensor<bst_group_t, 1> t;
    CopyTensorInfoImpl(ctx, arr, &t);
    bool non_dec = true;
    auto const& query_ids = t.Data()->HostVector();
    for (size_t i = 1; i < query_ids.size(); ++i) {
      if (query_ids[i] < query_ids[i - 1]) {
        non_dec = false;
        break;
      }
    }
    CHECK(non_dec) << "`qid` must be sorted in non-decreasing order along with data.";
    common::RunLengthEncode(query_ids.cbegin(), query_ids.cend(), &group_ptr_);
    data::ValidateQueryGroup(group_ptr_);
    return;
  }

  // float info
  linalg::Tensor<float, 1> t;
  CopyTensorInfoImpl<1>(ctx, arr, &t);
  if (key == "weight") {
    this->weights_ = std::move(*t.Data());
    auto const& h_weights = this->weights_.ConstHostVector();
    auto valid = std::none_of(h_weights.cbegin(), h_weights.cend(),
                              [](float w) { return w < 0 || std::isinf(w) || std::isnan(w); });
    CHECK(valid) << "Weights must be positive values.";
  } else if (key == "label_lower_bound") {
    this->labels_lower_bound_ = std::move(*t.Data());
  } else if (key == "label_upper_bound") {
    this->labels_upper_bound_ = std::move(*t.Data());
  } else if (key == "feature_weights") {
    this->feature_weights = std::move(*t.Data());
    auto const& h_feature_weights = feature_weights.ConstHostVector();
    bool valid =
        std::none_of(h_feature_weights.cbegin(), h_feature_weights.cend(), data::WeightsCheck{});
    CHECK(valid) << "Feature weight must be greater than 0.";
  } else {
    LOG(FATAL) << "Unknown key for MetaInfo: " << key;
  }
}

void MetaInfo::GetInfo(char const* key, bst_ulong* out_len, DataType dtype,
                       const void** out_dptr) const {
  if (dtype == DataType::kFloat32) {
    const std::vector<bst_float>* vec = nullptr;
    if (!std::strcmp(key, "label")) {
      vec = &this->labels.Data()->HostVector();
    } else if (!std::strcmp(key, "weight")) {
      vec = &this->weights_.HostVector();
    } else if (!std::strcmp(key, "base_margin")) {
      vec = &this->base_margin_.Data()->HostVector();
    } else if (!std::strcmp(key, "label_lower_bound")) {
      vec = &this->labels_lower_bound_.HostVector();
    } else if (!std::strcmp(key, "label_upper_bound")) {
      vec = &this->labels_upper_bound_.HostVector();
    } else if (!std::strcmp(key, "feature_weights")) {
      vec = &this->feature_weights.HostVector();
    } else {
      LOG(FATAL) << "Unknown float field name: " << key;
    }
    *out_len = static_cast<xgboost::bst_ulong>(vec->size()); // NOLINT
    *reinterpret_cast<float const**>(out_dptr) = dmlc::BeginPtr(*vec);
  } else if (dtype == DataType::kUInt32) {
    const std::vector<unsigned> *vec = nullptr;
    if (!std::strcmp(key, "group_ptr")) {
      vec = &this->group_ptr_;
    } else {
      LOG(FATAL) << "Unknown uint32 field name: " << key;
    }
    *out_len = static_cast<xgboost::bst_ulong>(vec->size());
    *reinterpret_cast<unsigned const**>(out_dptr) = dmlc::BeginPtr(*vec);
  } else {
    LOG(FATAL) << "Unknown data type for getting meta info.";
  }
}

void MetaInfo::SetFeatureInfo(const char* key, const char **info, const bst_ulong size) {
  bool is_col_split = this->IsColumnSplit();

  if (size != 0 && this->num_col_ != 0 && !is_col_split) {
    CHECK_EQ(size, this->num_col_) << "Length of " << key << " must be equal to number of columns.";
    CHECK(info);
  }

  // Gather column info when data is split by columns
  auto gather_columns = [is_col_split, key, n_columns = this->num_col_](auto const& inputs) {
    if (is_col_split) {
      std::remove_const_t<std::remove_reference_t<decltype(inputs)>> result;
      auto rc = collective::AllgatherStrings(inputs, &result);
      collective::SafeColl(rc);
      CHECK_EQ(result.size(), n_columns)
          << "Length of " << key << " must be equal to number of columns.";
      return result;
    }
    return inputs;
  };

  if (StringView{key} == "feature_type") {  // NOLINT
    this->feature_type_names.clear();
    std::copy(info, info + size, std::back_inserter(feature_type_names));
    feature_type_names = gather_columns(feature_type_names);
    auto& h_feature_types = feature_types.HostVector();
    this->has_categorical_ = LoadFeatureType(feature_type_names, &h_feature_types);
  } else if (StringView{key} == "feature_name") {  // NOLINT
    feature_names.clear();
    if (is_col_split) {
      auto const rank = collective::GetRank();
      std::transform(info, info + size, std::back_inserter(feature_names),
                     [rank](char const* elem) { return std::to_string(rank) + "." + elem; });
    } else {
      std::copy(info, info + size, std::back_inserter(feature_names));
    }
    feature_names = gather_columns(feature_names);
  } else {
    LOG(FATAL) << "Unknown feature info name: " << key;
  }
}

void MetaInfo::GetFeatureInfo(const char* field, std::vector<std::string>* out_str_vecs) const {
  auto& str_vecs = *out_str_vecs;
  if (!std::strcmp(field, "feature_type")) {
    str_vecs.resize(feature_type_names.size());
    std::copy(feature_type_names.cbegin(), feature_type_names.cend(), str_vecs.begin());
  } else if (!strcmp(field, "feature_name")) {
    str_vecs.resize(feature_names.size());
    std::copy(feature_names.begin(), feature_names.end(), str_vecs.begin());
  } else {
    LOG(FATAL) << "Unknown feature info: " << field;
  }
}

void MetaInfo::Extend(MetaInfo const& that, bool accumulate_rows, bool check_column) {
  /**
   * shape
   */
  if (accumulate_rows) {
    this->num_row_ += that.num_row_;
  }
  if (this->num_col_ != 0) {
    if (check_column) {
      CHECK_EQ(this->num_col_, that.num_col_)
          << "Number of columns must be consistent across batches.";
    } else {
      this->num_col_ = std::max(this->num_col_, that.num_col_);
    }
  }
  this->num_col_ = that.num_col_;

  /**
   * info with n_samples
   */
  linalg::Stack(&this->labels, that.labels);

  this->weights_.SetDevice(that.weights_.Device());
  this->weights_.Extend(that.weights_);

  this->labels_lower_bound_.SetDevice(that.labels_lower_bound_.Device());
  this->labels_lower_bound_.Extend(that.labels_lower_bound_);

  this->labels_upper_bound_.SetDevice(that.labels_upper_bound_.Device());
  this->labels_upper_bound_.Extend(that.labels_upper_bound_);

  linalg::Stack(&this->base_margin_, that.base_margin_);

  /**
   * group
   */
  if (this->group_ptr_.size() == 0) {
    this->group_ptr_ = that.group_ptr_;
  } else {
    CHECK_NE(that.group_ptr_.size(), 0);
    auto group_ptr = that.group_ptr_;
    for (size_t i = 1; i < group_ptr.size(); ++i) {
      group_ptr[i] += this->group_ptr_.back();
    }
    this->group_ptr_.insert(this->group_ptr_.end(), group_ptr.begin() + 1,
                            group_ptr.end());
  }

  /**
   * info with n_features
   */
  if (!that.feature_names.empty()) {
    this->feature_names = that.feature_names;
  }

  if (!this->feature_types.Empty()) {
    data::CheckFeatureTypes(this->feature_types, that.feature_types);
  }

  if (!that.feature_type_names.empty()) {
    this->feature_type_names = that.feature_type_names;
    auto& h_feature_types = feature_types.HostVector();
    this->has_categorical_ = LoadFeatureType(this->feature_type_names, &h_feature_types);
  } else if (!that.feature_types.Empty()) {
    // FIXME(jiamingy): https://github.com/dmlc/xgboost/pull/9171/files#r1440188612
    this->feature_types.Resize(that.feature_types.Size());
    this->feature_types.Copy(that.feature_types);
    auto const& ft = this->feature_types.ConstHostVector();
    this->has_categorical_ = std::any_of(ft.cbegin(), ft.cend(), common::IsCatOp{});
  }

  if (!that.feature_weights.Empty()) {
    this->feature_weights.Resize(that.feature_weights.Size());
    this->feature_weights.SetDevice(that.feature_weights.Device());
    this->feature_weights.Copy(that.feature_weights);
  }
}

void MetaInfo::SynchronizeNumberOfColumns(Context const* ctx, DataSplitMode split_mode) {
  this->data_split_mode = split_mode;
  auto op = IsColumnSplit() ? collective::Op::kSum : collective::Op::kMax;
  auto rc = collective::Allreduce(ctx, linalg::MakeVec(&num_col_, 1), op);
  collective::SafeColl(rc);
}

namespace {
template <typename T>
void CheckDevice(DeviceOrd device, HostDeviceVector<T> const& v) {
  bool valid = v.Device().IsCPU() || device.IsCPU() || v.Device() == device;
  if (!valid) {
    LOG(FATAL) << "Invalid device ordinal. Data is associated with a different device ordinal than "
                  "the booster. The device ordinal of the data is: "
               << v.Device() << "; the device ordinal of the Booster is: " << device;
  }
}

template <typename T, std::int32_t D>
void CheckDevice(DeviceOrd device, linalg::Tensor<T, D> const& v) {
  CheckDevice(device, *v.Data());
}
}  // anonymous namespace

void MetaInfo::Validate(DeviceOrd device) const {
  if (group_ptr_.size() != 0 && weights_.Size() != 0) {
    CHECK_EQ(group_ptr_.size(), weights_.Size() + 1) << error::GroupWeight();
    return;
  }
  if (group_ptr_.size() != 0) {
    CHECK_EQ(group_ptr_.back(), num_row_)
        << error::GroupSize() << "the actual number of rows given by data.";
  }

  if (weights_.Size() != 0) {
    CHECK_EQ(weights_.Size(), num_row_)
        << "Size of weights must equal to number of rows.";
    CheckDevice(device, weights_);
    return;
  }
  if (labels.Size() != 0) {
    CHECK_EQ(labels.Shape(0), num_row_) << "Size of labels must equal to number of rows.";
    CheckDevice(device, labels);
    return;
  }
  if (labels_lower_bound_.Size() != 0) {
    CHECK_EQ(labels_lower_bound_.Size(), num_row_)
        << "Size of label_lower_bound must equal to number of rows.";
    CheckDevice(device, labels_lower_bound_);
    return;
  }
  if (feature_weights.Size() != 0) {
    CHECK_EQ(feature_weights.Size(), num_col_)
        << "Size of feature_weights must equal to number of columns.";
    CheckDevice(device, feature_weights);
  }
  if (labels_upper_bound_.Size() != 0) {
    CHECK_EQ(labels_upper_bound_.Size(), num_row_)
        << "Size of label_upper_bound must equal to number of rows.";
    CheckDevice(device, labels_upper_bound_);
    return;
  }
  CHECK_LE(num_nonzero_, num_col_ * num_row_);
  if (base_margin_.Size() != 0) {
    CHECK_EQ(base_margin_.Size() % num_row_, 0)
        << "Size of base margin must be a multiple of number of rows.";
    CheckDevice(device, base_margin_);
  }
}

#if !defined(XGBOOST_USE_CUDA)
void MetaInfo::SetInfoFromCUDA(Context const*, StringView, Json) { common::AssertGPUSupport(); }
#endif  // !defined(XGBOOST_USE_CUDA)

bool MetaInfo::IsVerticalFederated() const {
  return collective::IsFederated() && IsColumnSplit();
}

bool MetaInfo::ShouldHaveLabels() const {
  return !IsVerticalFederated() || collective::GetRank() == 0;
}

[[nodiscard]] CatContainer const* MetaInfo::Cats() const { return this->cats_.get(); }
[[nodiscard]] CatContainer* MetaInfo::Cats() { return this->cats_.get(); }

[[nodiscard]] std::shared_ptr<CatContainer const> MetaInfo::CatsShared() const {
  return this->cats_;
}

void MetaInfo::Cats(std::shared_ptr<CatContainer> cats) {
  this->cats_ = std::move(cats);
  CHECK_LT(cats_->NumFeatures(),
           static_cast<decltype(cats->NumFeatures())>(std::numeric_limits<bst_cat_t>::max()));
}

using DMatrixThreadLocal =
    dmlc::ThreadLocalStore<std::map<DMatrix const *, XGBAPIThreadLocalEntry>>;

XGBAPIThreadLocalEntry& DMatrix::GetThreadLocal() const {
  return (*DMatrixThreadLocal::Get())[this];
}

DMatrix::~DMatrix() {
  auto local_map = DMatrixThreadLocal::Get();
  if (local_map->find(this) != local_map->cend()) {
    local_map->erase(this);
  }
}

namespace {
DMatrix* TryLoadBinary(std::string fname, bool silent) {
  std::int32_t magic;
  std::unique_ptr<dmlc::Stream> fi(dmlc::Stream::Create(fname.c_str(), "r", true));
  if (fi != nullptr) {
    common::PeekableInStream is(fi.get());
    if (is.PeekRead(&magic, sizeof(magic)) == sizeof(magic)) {
      if (!DMLC_IO_NO_ENDIAN_SWAP) {
        dmlc::ByteSwap(&magic, sizeof(magic), 1);
      }
      if (magic == data::SimpleDMatrix::kMagic) {
        DMatrix* dmat = new data::SimpleDMatrix(&is);
        if (!silent) {
          LOG(INFO) << dmat->Info().num_row_ << 'x' << dmat->Info().num_col_ << " matrix with "
                    << dmat->Info().num_nonzero_ << " entries loaded from " << fname;
        }
        return dmat;
      }
    }
  }
  return nullptr;
}
}  // namespace

DMatrix* DMatrix::Load(const std::string& uri, bool silent, DataSplitMode data_split_mode) {
  auto dlm_pos = uri.find('#');
  CHECK(dlm_pos == std::string::npos)
      << "External memory training with text input has been removed.";
  std::string fname = uri;

  // legacy handling of binary data loading
  DMatrix* loaded = TryLoadBinary(fname, silent);
  if (loaded) {
    return loaded;
  }

  int partid = 0, npart = 1;

  static std::once_flag warning_flag;
  std::call_once(warning_flag, []() {
    LOG(WARNING) << "Text file input has been deprecated since 3.1";
  });

  fname = data::ValidateFileFormat(fname);
  std::unique_ptr<dmlc::Parser<std::uint32_t>> parser(
      dmlc::Parser<std::uint32_t>::Create(fname.c_str(), partid, npart, "auto"));
  data::FileAdapter adapter(parser.get());
  return DMatrix::Create(&adapter, std::numeric_limits<float>::quiet_NaN(), Context{}.Threads(), "",
                         data_split_mode);
}

template <typename DataIterHandle, typename DMatrixHandle, typename DataIterResetCallback,
          typename XGDMatrixCallbackNext>
DMatrix* DMatrix::Create(DataIterHandle iter, DMatrixHandle proxy, std::shared_ptr<DMatrix> ref,
                         DataIterResetCallback* reset, XGDMatrixCallbackNext* next, float missing,
                         int nthread, bst_bin_t max_bin, std::int64_t max_quantile_blocks) {
  return new data::IterativeDMatrix(iter, proxy, ref, reset, next, missing, nthread, max_bin,
                                    max_quantile_blocks);
}

template <typename DataIterHandle, typename DMatrixHandle, typename DataIterResetCallback,
          typename XGDMatrixCallbackNext>
DMatrix* DMatrix::Create(DataIterHandle iter, DMatrixHandle proxy, DataIterResetCallback* reset,
                         XGDMatrixCallbackNext* next, ExtMemConfig const& config) {
  return new data::SparsePageDMatrix{iter, proxy, reset, next, config};
}

template <typename DataIterHandle, typename DMatrixHandle, typename DataIterResetCallback,
          typename XGDMatrixCallbackNext>
DMatrix* DMatrix::Create(DataIterHandle iter, DMatrixHandle proxy, std::shared_ptr<DMatrix> ref,
                         DataIterResetCallback* reset, XGDMatrixCallbackNext* next,
                         bst_bin_t max_bin, std::int64_t max_quantile_blocks,
                         ExtMemConfig const& config) {
  return new data::ExtMemQuantileDMatrix{
      iter, proxy, ref, reset, next, max_bin, max_quantile_blocks, config};
}

template DMatrix*
DMatrix::Create<DataIterHandle, DMatrixHandle, DataIterResetCallback, XGDMatrixCallbackNext>(
    DataIterHandle iter, DMatrixHandle proxy, std::shared_ptr<DMatrix> ref,
    DataIterResetCallback* reset, XGDMatrixCallbackNext* next, float missing, int nthread,
    int max_bin, std::int64_t max_quantile_blocks);

template DMatrix* DMatrix::Create<DataIterHandle, DMatrixHandle, DataIterResetCallback,
                                  XGDMatrixCallbackNext>(DataIterHandle iter, DMatrixHandle proxy,
                                                         DataIterResetCallback* reset,
                                                         XGDMatrixCallbackNext* next,
                                                         ExtMemConfig const&);

template DMatrix*
DMatrix::Create<DataIterHandle, DMatrixHandle, DataIterResetCallback, XGDMatrixCallbackNext>(
    DataIterHandle, DMatrixHandle, std::shared_ptr<DMatrix>, DataIterResetCallback*,
    XGDMatrixCallbackNext*, bst_bin_t, std::int64_t, ExtMemConfig const&);

template <typename AdapterT>
DMatrix* DMatrix::Create(AdapterT* adapter, float missing, int nthread, const std::string&,
                         DataSplitMode data_split_mode) {
  return new data::SimpleDMatrix(adapter, missing, nthread, data_split_mode);
}

// Instantiate the factory function for various adapters
#define INSTANTIATION_CREATE(_AdapterT)                               \
  template DMatrix* DMatrix::Create<data::_AdapterT>(                 \
      data::_AdapterT * adapter, float missing, std::int32_t nthread, \
      const std::string& cache_prefix, DataSplitMode data_split_mode);

INSTANTIATION_CREATE(DenseAdapter)
INSTANTIATION_CREATE(ArrayAdapter)
INSTANTIATION_CREATE(FileAdapter)
INSTANTIATION_CREATE(CSRArrayAdapter)
INSTANTIATION_CREATE(CSCArrayAdapter)
INSTANTIATION_CREATE(ColumnarAdapter)

#undef INSTANTIATION_CREATE

template DMatrix* DMatrix::Create(
    data::IteratorAdapter<DataIterHandle, XGBCallbackDataIterNext, XGBoostBatchCSR>* adapter,
    float missing, int nthread, std::string const& cache_prefix, DataSplitMode data_split_mode);

SparsePage SparsePage::GetTranspose(int num_columns, int32_t n_threads) const {
  SparsePage transpose;
  common::ParallelGroupBuilder<Entry, bst_idx_t> builder(&transpose.offset.HostVector(),
                                                         &transpose.data.HostVector());
  builder.InitBudget(num_columns, n_threads);
  long batch_size = static_cast<long>(this->Size());  // NOLINT(*)
  auto page = this->GetView();
  common::ParallelFor(batch_size, n_threads, [&](long i) {  // NOLINT(*)
    int tid = omp_get_thread_num();
    auto inst = page[i];
    for (const auto& entry : inst) {
      builder.AddBudget(entry.index, tid);
    }
  });
  builder.InitStorage();
  common::ParallelFor(batch_size, n_threads, [&](long i) {  // NOLINT(*)
    int tid = omp_get_thread_num();
    auto inst = page[i];
    for (const auto& entry : inst) {
      builder.Push(
          entry.index,
          Entry(static_cast<bst_uint>(this->base_rowid + i), entry.fvalue),
          tid);
    }
  });

  if (this->data.Empty()) {
    transpose.offset.Resize(num_columns + 1);
    transpose.offset.Fill(0);
  }
  CHECK_EQ(transpose.offset.Size(), num_columns + 1);
  return transpose;
}

bool SparsePage::IsIndicesSorted(int32_t n_threads) const {
  auto& h_offset = this->offset.HostVector();
  auto& h_data = this->data.HostVector();
  n_threads = std::max(std::min(static_cast<std::size_t>(n_threads), this->Size()),
                       static_cast<std::size_t>(1));
  std::vector<int32_t> is_sorted_tloc(n_threads, 0);
  common::ParallelFor(this->Size(), n_threads, [&](auto i) {
    auto beg = h_offset[i];
    auto end = h_offset[i + 1];
    is_sorted_tloc[omp_get_thread_num()] +=
        !!std::is_sorted(h_data.begin() + beg, h_data.begin() + end, Entry::CmpIndex);
  });
  auto is_sorted = std::accumulate(is_sorted_tloc.cbegin(), is_sorted_tloc.cend(),
                                   static_cast<size_t>(0)) == this->Size();
  return is_sorted;
}

void SparsePage::SortIndices(int32_t n_threads) {
  auto& h_offset = this->offset.HostVector();
  auto& h_data = this->data.HostVector();

  common::ParallelFor(this->Size(), n_threads, [&](auto i) {
    auto beg = h_offset[i];
    auto end = h_offset[i + 1];
    std::sort(h_data.begin() + beg, h_data.begin() + end, Entry::CmpIndex);
  });
}

void SparsePage::Reindex(uint64_t feature_offset, int32_t n_threads) {
  auto& h_data = this->data.HostVector();
  common::ParallelFor(h_data.size(), n_threads, [&](auto i) {
    h_data[i].index += feature_offset;
  });
}

void SparsePage::SortRows(int32_t n_threads) {
  auto& h_offset = this->offset.HostVector();
  auto& h_data = this->data.HostVector();
  common::ParallelFor(this->Size(), n_threads, [&](auto i) {
    if (h_offset[i] < h_offset[i + 1]) {
      std::sort(h_data.begin() + h_offset[i], h_data.begin() + h_offset[i + 1], Entry::CmpValue);
    }
  });
}

void SparsePage::Push(const SparsePage &batch) {
  auto& data_vec = data.HostVector();
  auto& offset_vec = offset.HostVector();
  const auto& batch_offset_vec = batch.offset.HostVector();
  const auto& batch_data_vec = batch.data.HostVector();
  size_t top = offset_vec.back();
  data_vec.resize(top + batch.data.Size());
  if (dmlc::BeginPtr(data_vec) && dmlc::BeginPtr(batch_data_vec)) {
    std::memcpy(dmlc::BeginPtr(data_vec) + top, dmlc::BeginPtr(batch_data_vec),
                sizeof(Entry) * batch.data.Size());
  }
  size_t begin = offset.Size();
  offset_vec.resize(begin + batch.Size());
  for (size_t i = 0; i < batch.Size(); ++i) {
    offset_vec[i + begin] = top + batch_offset_vec[i + 1];
  }
}

template <typename AdapterBatchT>
bst_idx_t SparsePage::Push(AdapterBatchT const& batch, float missing, std::int32_t nthread) {
  constexpr bool kIsRowMajor = AdapterBatchT::kIsRowMajor;
  // Allow threading only for row-major case as column-major requires O(nthread*batch_size) memory
  nthread = kIsRowMajor ? nthread : 1;
  if (!kIsRowMajor) {
    CHECK_EQ(nthread, 1);
  }
  auto& offset_vec = offset.HostVector();
  auto& data_vec = data.HostVector();

  size_t builder_base_row_offset = this->Size();
  common::ParallelGroupBuilder<
      Entry, std::remove_reference<decltype(offset_vec)>::type::value_type, kIsRowMajor>
      builder(&offset_vec, &data_vec, builder_base_row_offset);
  // Estimate expected number of rows by using last element in batch
  // This is not required to be exact but prevents unnecessary resizing
  size_t expected_rows = 0;
  if (batch.Size() > 0) {
    auto last_line = batch.GetLine(batch.Size() - 1);
    if (last_line.Size() > 0) {
      expected_rows =
          last_line.GetElement(last_line.Size() - 1).row_idx - base_rowid;
    }
  }
  size_t batch_size = batch.Size();
  expected_rows = kIsRowMajor ? batch_size : expected_rows;
  uint64_t max_columns = 0;
  if (batch_size == 0) {
    return max_columns;
  }
  const size_t thread_size = batch_size / nthread;

  builder.InitBudget(expected_rows, nthread);
  std::vector<std::vector<uint64_t>> max_columns_vector(nthread, std::vector<uint64_t>{0});
  dmlc::OMPException exec;
  std::atomic<bool> valid{true};
  // First-pass over the batch counting valid elements
#pragma omp parallel num_threads(nthread)
  {
    exec.Run([&]() {
      int tid = omp_get_thread_num();
      size_t begin = tid*thread_size;
      size_t end = tid != (nthread-1) ? (tid+1)*thread_size : batch_size;
      uint64_t& max_columns_local = max_columns_vector[tid][0];

      for (size_t i = begin; i < end; ++i) {
        auto line = batch.GetLine(i);
        for (auto j = 0ull; j < line.Size(); j++) {
          data::COOTuple const& element = line.GetElement(j);
          if (!std::isinf(missing) && std::isinf(element.value)) {
            valid = false;
          }
          const size_t key = element.row_idx - base_rowid;
          CHECK_GE(key,  builder_base_row_offset);
          max_columns_local =
              std::max(max_columns_local, static_cast<uint64_t>(element.column_idx + 1));

          if (!common::CheckNAN(element.value) && element.value != missing) {
            // Adapter row index is absolute, here we want it relative to
            // current page
            builder.AddBudget(key, tid);
          }
        }
      }
    });
  }
  exec.Rethrow();
  CHECK(valid) << error::InfInData();
  for (const auto & max : max_columns_vector) {
    max_columns = std::max(max_columns, max[0]);
  }

  builder.InitStorage();

  // Second pass over batch, placing elements in correct position
  auto is_valid = data::IsValidFunctor{missing};
#pragma omp parallel num_threads(nthread)
  {
    exec.Run([&]() {
      int tid = omp_get_thread_num();
      size_t begin = tid * thread_size;
      size_t end = tid != (nthread - 1) ? (tid + 1) * thread_size : batch_size;
      for (size_t i = begin; i < end; ++i) {
        auto line = batch.GetLine(i);
        for (auto j = 0ull; j < line.Size(); j++) {
          auto element = line.GetElement(j);
          const size_t key = (element.row_idx - base_rowid);
          if (is_valid(element)) {
            builder.Push(key, Entry(element.column_idx, element.value), tid);
          }
        }
      }
    });
  }
  exec.Rethrow();
  return max_columns;
}

void SparsePage::PushCSC(const SparsePage &batch) {
  std::vector<xgboost::Entry>& self_data = data.HostVector();
  std::vector<bst_idx_t>& self_offset = offset.HostVector();

  auto const& other_data = batch.data.ConstHostVector();
  auto const& other_offset = batch.offset.ConstHostVector();

  if (other_data.empty()) {
    self_offset = other_offset;
    return;
  }
  if (!self_data.empty()) {
    CHECK_EQ(self_offset.size(), other_offset.size())
        << "self_data.size(): " << this->data.Size() << ", "
        << "other_data.size(): " << other_data.size() << std::flush;
  } else {
    self_data = other_data;
    self_offset = other_offset;
    return;
  }

  std::vector<bst_idx_t> offset(other_offset.size());
  offset[0] = 0;

  std::vector<xgboost::Entry> data(self_data.size() + other_data.size());

  // n_cols in original csr data matrix, here in csc is n_rows
  size_t const n_features = other_offset.size() - 1;
  size_t beg = 0;
  size_t ptr = 1;
  for (size_t i = 0; i < n_features; ++i) {
    size_t const self_beg = self_offset.at(i);
    size_t const self_length = self_offset.at(i+1) - self_beg;
    // It is possible that the current feature and further features aren't referenced
    // in any rows accumulated thus far. It is also possible for this to happen
    // in the current sparse page row batch as well.
    // Hence, the incremental number of rows may stay constant thus equaling the data size
    CHECK_LE(beg, data.size());
    std::memcpy(dmlc::BeginPtr(data)+beg,
                dmlc::BeginPtr(self_data) + self_beg,
                sizeof(Entry) * self_length);
    beg += self_length;

    size_t const other_beg = other_offset.at(i);
    size_t const other_length = other_offset.at(i+1) - other_beg;
    CHECK_LE(beg, data.size());
    std::memcpy(dmlc::BeginPtr(data)+beg,
                dmlc::BeginPtr(other_data) + other_beg,
                sizeof(Entry) * other_length);
    beg += other_length;

    CHECK_LT(ptr, offset.size());
    offset.at(ptr) = beg;
    ptr++;
  }

  self_data = std::move(data);
  self_offset = std::move(offset);
}

#define INSTANTIATE_PUSH(__BATCH_T)                                                    \
  template std::uint64_t SparsePage::Push(const data::__BATCH_T& batch, float missing, \
                                          std::int32_t nthread);

INSTANTIATE_PUSH(DenseAdapterBatch)
INSTANTIATE_PUSH(ArrayAdapterBatch)
INSTANTIATE_PUSH(CSRArrayAdapterBatch)
INSTANTIATE_PUSH(CSCArrayAdapterBatch)
INSTANTIATE_PUSH(FileAdapterBatch)
INSTANTIATE_PUSH(ColumnarAdapterBatch)

#undef INSTANTIATE_PUSH

namespace data {
// List of files that will be force linked in static links.
DMLC_REGISTRY_LINK_TAG(sparse_page_raw_format);
DMLC_REGISTRY_LINK_TAG(gradient_index_format);
}  // namespace data
}  // namespace xgboost
