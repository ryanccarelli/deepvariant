/*
 * Copyright 2018 Google LLC.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

// Implementation of vcf_writer.h
#include "third_party/nucleus/io/vcf_writer.h"

#include <cmath>
#include <utility>
#include <vector>

#include "google/protobuf/map.h"
#include "google/protobuf/repeated_field.h"
#include "absl/memory/memory.h"
#include "absl/strings/substitute.h"
#include "htslib/hts.h"
#include "htslib/sam.h"
#include "third_party/nucleus/io/hts_path.h"
#include "third_party/nucleus/io/vcf_conversion.h"
#include "third_party/nucleus/protos/reference.pb.h"
#include "third_party/nucleus/protos/variants.pb.h"
#include "third_party/nucleus/util/utils.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/logging.h"

namespace nucleus {

namespace tf = tensorflow;

using nucleus::genomics::v1::Variant;

namespace {

// Modes used to write a htsFile. See hts_open in hts.h for more info.
constexpr char kBCFOpenModeCompressed[] = "wb";
constexpr char kBCFOpenModeUncompressed[] = "wbu";
constexpr char kOpenModeCompressed[] = "wz";
constexpr char kOpenModeUncompressed[] = "w";

constexpr char kFilterHeaderFmt[] = "##FILTER=<ID=$0,Description=\"$1\">";
constexpr char kInfoHeaderFmt[] =
    "##INFO=<ID=$0,Number=$1,Type=$2,Description=\"$3\"$4>";
constexpr char kFormatHeaderFmt[] =
    "##FORMAT=<ID=$0,Number=$1,Type=$2,Description=\"$3\">";
constexpr char kContigHeaderFmt[] = "##contig=<ID=$0$1>";
constexpr char kStructuredExtraHeaderFmt[] = "##$0=<$1>";
constexpr char kExtraHeaderFmt[] = "##$0=$1";

// Adds a FILTER field to the bcf_hdr_t header based on the VcfFilterInfo
// object.
void AddFilterToHeader(const nucleus::genomics::v1::VcfFilterInfo& filter,
                       bcf_hdr_t* header) {
  string filterStr = absl::Substitute(
      kFilterHeaderFmt, filter.id().c_str(), filter.description().c_str());
  bcf_hdr_append(header, filterStr.c_str());
}

// Adds an INFO field to the bcf_hdr_t header based on the VcfInfo object.
void AddInfoToHeader(const nucleus::genomics::v1::VcfInfo& info,
                     bcf_hdr_t* header) {
  string extra;
  if (!info.source().empty()) {
    absl::StrAppend(&extra, ",Source=\"", info.source(), "\"");
  }
  if (!info.version().empty()) {
    absl::StrAppend(&extra, ",Version=\"", info.version(), "\"");
  }
  string infoStr = absl::Substitute(
      kInfoHeaderFmt, info.id().c_str(), info.number().c_str(),
      info.type().c_str(), info.description().c_str(), extra.c_str());
  bcf_hdr_append(header, infoStr.c_str());
}

// Adds a FORMAT field to the bcf_hdr_t header based on the VcfFormatInfo
// object.
void AddFormatToHeader(const nucleus::genomics::v1::VcfFormatInfo& format,
                       bcf_hdr_t* header) {
  string formatStr = absl::Substitute(
      kFormatHeaderFmt, format.id().c_str(), format.number().c_str(),
      format.type().c_str(), format.description().c_str());
  bcf_hdr_append(header, formatStr.c_str());
}

// Adds a structured extra field to the bcf_hdr_t header based on the
// VcfStructuredExtra object.
void AddStructuredExtraToHeader(
    const nucleus::genomics::v1::VcfStructuredExtra& sExtra,
    bcf_hdr_t* header) {
  string fieldStr;
  for (auto const& kv : sExtra.fields()) {
    absl::StrAppend(&fieldStr, kv.key(), "=\"", kv.value(), "\",");
  }
  if (!fieldStr.empty()) {
    // Cut off the dangling comma.
    fieldStr.pop_back();
  }
  string result = absl::Substitute(
      kStructuredExtraHeaderFmt, sExtra.key().c_str(), fieldStr.c_str());
  bcf_hdr_append(header, result.c_str());
}

// Adds an unstructured extra field to the bcf_hdr_t header based on the
// VcfExtra object.
void AddExtraToHeader(const nucleus::genomics::v1::VcfExtra& extra,
                      bcf_hdr_t* header) {
  string result = absl::Substitute(
      kExtraHeaderFmt, extra.key().c_str(), extra.value().c_str());
  bcf_hdr_append(header, result.c_str());
}

// Adds a contig field to the bcf_hdr_t header based on the ContigInfo object.
void AddContigToHeader(const nucleus::genomics::v1::ContigInfo& contig,
                       bcf_hdr_t* header) {
  string extra;
  if (contig.n_bases()) {
    absl::StrAppend(&extra, ",length=", contig.n_bases());
  }
  if (!contig.description().empty()) {
    absl::StrAppend(&extra, ",description=\"", contig.description(), "\"");
  }
  for (auto const& kv : contig.extra()) {
    absl::StrAppend(&extra, ",", kv.first, "=\"", kv.second, "\"");
  }
  string contigStr = absl::Substitute(
      kContigHeaderFmt, contig.name().c_str(), extra.c_str());
  bcf_hdr_append(header, contigStr.c_str());
}

// RAII wrapper on top of bcf1_t* to always perform cleanup.
class BCFRecord {
 public:
  BCFRecord() : bcf1_(bcf_init()) {}
  ~BCFRecord() {
    if (bcf1_) {
      bcf_destroy(bcf1_);
    }
  }

  // Disable copy or assignment
  BCFRecord(const BCFRecord& other) = delete;
  BCFRecord& operator=(const BCFRecord&) = delete;

  bcf1_t* get_bcf1() { return bcf1_; }

 private:
  bcf1_t* bcf1_;
};

}  // namespace

StatusOr<std::unique_ptr<VcfWriter>> VcfWriter::ToFile(
    const string& variants_path, const nucleus::genomics::v1::VcfHeader& header,
    const nucleus::genomics::v1::VcfWriterOptions& options) {
  const char* const open_mode = GetOpenMode(variants_path);
  htsFile* fp = hts_open_x(variants_path.c_str(), open_mode);
  if (fp == nullptr) {
    return tf::errors::Unknown("Could not open variants_path", variants_path);
  }

  auto writer = absl::WrapUnique(new VcfWriter(header, options, fp));
  TF_RETURN_IF_ERROR(writer->WriteHeader());
  return std::move(writer);
}

VcfWriter::VcfWriter(const nucleus::genomics::v1::VcfHeader& header,
                     const nucleus::genomics::v1::VcfWriterOptions& options,
                     htsFile* fp)
    : fp_(fp),
      options_(options),
      vcf_header_(header),
      record_converter_(
          vcf_header_,
          std::vector<string>(options_.excluded_info_fields().begin(),
                              options_.excluded_info_fields().end()),
          std::vector<string>(options_.excluded_format_fields().begin(),
                              options_.excluded_format_fields().end()),
          options_.retrieve_gl_and_pl_from_info_map()) {
  CHECK(fp != nullptr);

  // Note: bcf_hdr_init writes the fileformat= and the FILTER=<ID=PASS,...>
  // filter automatically.
  header_ = bcf_hdr_init("w");
  for (const nucleus::genomics::v1::VcfFilterInfo& filter :
       vcf_header_.filters()) {
    if (filter.id() != "PASS") {
      AddFilterToHeader(filter, header_);
    }
  }
  for (const nucleus::genomics::v1::VcfInfo& info : vcf_header_.infos()) {
    AddInfoToHeader(info, header_);
  }
  for (const nucleus::genomics::v1::VcfFormatInfo& format :
       vcf_header_.formats()) {
    AddFormatToHeader(format, header_);
  }
  for (const nucleus::genomics::v1::VcfStructuredExtra& sExtra :
       vcf_header_.structured_extras()) {
    AddStructuredExtraToHeader(sExtra, header_);
  }
  for (const nucleus::genomics::v1::VcfExtra& extra : vcf_header_.extras()) {
    AddExtraToHeader(extra, header_);
  }
  for (const nucleus::genomics::v1::ContigInfo& contig :
       vcf_header_.contigs()) {
    AddContigToHeader(contig, header_);
  }

  for (const string& sampleName : vcf_header_.sample_names()) {
    bcf_hdr_add_sample(header_, sampleName.c_str());
  }
  bcf_hdr_add_sample(header_, nullptr);
}

tf::Status VcfWriter::WriteHeader() {
  if (bcf_hdr_write(fp_, header_) < 0)
    return tf::errors::Unknown("Failed to write header");
  else
    return tf::Status::OK();
}

VcfWriter::~VcfWriter() {
  if (fp_) {
    // There's nothing we can do but assert fail if there's an error during
    // the Close() call here.
    TF_CHECK_OK(Close());
  }
}

tf::Status VcfWriter::Write(const Variant& variant_message) {
  if (fp_ == nullptr)
    return tf::errors::FailedPrecondition("Cannot write to closed VCF stream.");
  BCFRecord v;
  if (v.get_bcf1() == nullptr) {
    return tf::errors::Unknown("bcf_init call failed");
  }
  TF_RETURN_IF_ERROR(
      RecordConverter().ConvertFromPb(variant_message, *header_, v.get_bcf1()));
  if (options_.round_qual_values() &&
      !bcf_float_is_missing(v.get_bcf1()->qual)) {
    // Round quality value printed out to one digit past the decimal point.
    double rounded_quality = floor(variant_message.quality() * 10 + 0.5) / 10;
    v.get_bcf1()->qual = rounded_quality;
  }
  if (bcf_write(fp_, header_, v.get_bcf1()) != 0) {
    return tf::errors::Unknown("bcf_write call failed");
  }
  return tf::Status::OK();
}

tf::Status VcfWriter::Close() {
  if (fp_ == nullptr)
    return tf::errors::FailedPrecondition(
        "Cannot close an already closed VcfWriter");
  if (hts_close(fp_) < 0)
    return tf::errors::Unknown("hts_close call failed");
  fp_ = nullptr;
  bcf_hdr_destroy(header_);
  header_ = nullptr;
  return tf::Status::OK();
}

// static
const char* VcfWriter::GetOpenMode(const string& file_path) {
  if (EndsWith(file_path, ".bcf")) {
    return kBCFOpenModeUncompressed;
  } else if (EndsWith(file_path, ".bcf.gz")) {
    return kBCFOpenModeCompressed;
  } else if (EndsWith(file_path, ".gz")) {
    return kOpenModeCompressed;
  }
  return kOpenModeUncompressed;
}

}  // namespace nucleus
