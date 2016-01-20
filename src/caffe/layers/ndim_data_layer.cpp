#include <fstream>  // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <string>
#include <utility>
#include <vector>

#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>

#include "caffe/data_transformer.hpp"
#include "caffe/layers/base_data_layer.hpp"
#include "caffe/layers/ndim_data_layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"



namespace caffe {

template <typename Dtype>
NDimDataLayer<Dtype>::~NDimDataLayer<Dtype>() {
  this->StopInternalThread();
}

//create simple shape vector from blobshape
template <typename Dtype>
const vector<int>  NDimDataLayer<Dtype>::blob2vec(const BlobShape& b) const
{
  CHECK_LE(b.dim_size(), kMaxBlobAxes);
  vector<int> shape_vec(b.dim_size());
  for (int i = 0, n = b.dim_size(); i < n; ++i) {
    shape_vec[i] = b.dim(i);
  }
  return shape_vec;
}

template <typename Dtype>
void NDimDataLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {

  string root_folder = this->layer_param_.ndim_data_param().root_folder();
  const bool balanced  = this->layer_param_.ndim_data_param().balanced();

  all_pos_ = actives_pos_ = decoys_pos_ = 0;
  // Read the file with filenames and labels
  const string& source = this->layer_param_.ndim_data_param().source();
  LOG(INFO) << "Opening file " << source;
  std::ifstream infile(source.c_str());
  string line, fname;
  vector<std::string> binmaps;

  while (getline(infile, line)) {
    stringstream example(line);
    int label = 0;
    //first the label
    example >> label;
    //then all binmaps for the example
    binmaps.clear();

    while(example >> fname)
      binmaps.push_back(fname);

    if(binmaps.size() == 0) //ignore empty lines
      continue;
      
    all_.push_back(make_pair(binmaps,label));
    if(label) actives_.push_back(binmaps);
    else decoys_.push_back(binmaps);
  }

  if (this->layer_param_.ndim_data_param().shuffle()) {
    // randomly shuffle data
    LOG(INFO) << "Shuffling data";
    const unsigned int prefetch_rng_seed = caffe_rng_rand();
    prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
    Shuffle();
  }
  LOG(INFO) << "A total of " << all_.size() << " examples.";

  // Check if we would need to randomly skip a few data points
  if (this->layer_param_.ndim_data_param().rand_skip()) {
    unsigned int skip = caffe_rng_rand() %
        this->layer_param_.ndim_data_param().rand_skip();
    LOG(INFO) << "Skipping first " << skip << " data points.";
    CHECK_GT(all_.size(), skip) << "Not enough points to skip";
    all_pos_ = skip;
    actives_pos_ = skip % actives_.size();
    decoys_pos_ = skip % decoys_.size();

  }

  //shape must come from parameters
  const int batch_size = this->layer_param_.ndim_data_param().batch_size();
  CHECK_GT(batch_size, 0) << "Positive batch size required";

  if(balanced) {
    CHECK_GT(batch_size, 1) << "Batch size must be > 1 with balanced option.";
  }

  vector<int> example_shape = blob2vec(this->layer_param_.ndim_data_param().shape());
  top_shape.clear();
  top_shape.push_back(1);

  example_size =1;
  for(unsigned i = 0, n = example_shape.size(); i < n; i++) {
    CHECK_GT(example_shape[i], 0) << "Positive shape dimension required";
    top_shape.push_back(example_shape[i]);
    example_size *= example_shape[i];
  }
  //shape of single data
  this->transformed_data_.Reshape(top_shape);

  // Reshape prefetch_data and top[0] according to the batch_size.
  top_shape[0] = batch_size;
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].data_.Reshape(top_shape);
  }
  top[0]->Reshape(top_shape);

  // label
  vector<int> label_shape(1, batch_size);
  top[1]->Reshape(label_shape);
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].label_.Reshape(label_shape);
  }
}

template <typename Dtype>
void NDimDataLayer<Dtype>::Shuffle() {
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
    shuffle(actives_.begin(), actives_.end(), prefetch_rng);
    shuffle(decoys_.begin(), decoys_.end(), prefetch_rng);
    shuffle(all_.begin(), all_.end(), prefetch_rng);
}

//copy raw floating point data from files into buffer
//files should represent a single example
template <typename Dtype>
void  NDimDataLayer<Dtype>::load_data_from_files(Dtype* buffer, const std::string& root, const vector<std::string>& files)
{
  using namespace boost::iostreams;
  basic_array_sink<char> data((char*)buffer, example_size*sizeof(Dtype));

  CHECK_GT(files.size(), 0) << "Missing binmaps files";
  
  unsigned long total = 0;
  for(unsigned i = 0, n = files.size(); i < n; i++)
  {
    //TODO, support gzip
    std::string fname = root + files[i];


    filtering_stream<input> in;
    std::ifstream inorig(fname.c_str());
    std::string::size_type pos = fname.rfind(".gz");
    if (pos != std::string::npos)
    {
      in.push(gzip_decompressor());
    }
    in.push(inorig);

    CHECK(in) << "Could not load " << fname;

    total += copy(in, data);
  }
  CHECK_EQ(total,example_size*sizeof(Dtype)) << "Incorrect size of inputs (" << total << " vs. " << example_size*sizeof(Dtype) << ") on " << files[0];
}


// This function is called on prefetch thread
template <typename Dtype>
void NDimDataLayer<Dtype>::load_batch(Batch<Dtype>* batch) {
  CPUTimer batch_timer;
  batch_timer.Start();
  string root_folder = this->layer_param_.ndim_data_param().root_folder();
  const bool balanced  = this->layer_param_.ndim_data_param().balanced();

  CHECK(batch->data_.count());
  CHECK(this->transformed_data_.count());
  unsigned batch_size = top_shape[0];
  CHECK_GT(batch_size, 0) << "Positive batch size required";
  vector<int> offind(1, 0);

  batch->data_.Reshape(top_shape);

  Dtype* prefetch_data = batch->data_.mutable_cpu_data();
  Dtype* prefetch_label = batch->label_.mutable_cpu_data();

  if(balanced) { //load equally from actives/decoys
    unsigned nactives = batch_size/2;

    int item_id = 0;
    unsigned asz = actives_.size();
    for (item_id = 0; item_id < nactives; ++item_id) {
      offind[0] = item_id;
      int offset = batch->data_.offset(offind);
      load_data_from_files(prefetch_data+offset, root_folder, actives_[actives_pos_]);
      prefetch_label[item_id] = 1;

      actives_pos_++;
      if(actives_pos_ >= asz) {
        DLOG(INFO) << "Restarting actives data prefetching from start.";
        actives_pos_ = 0;
        if (this->layer_param_.ndim_data_param().shuffle()) {
          shuffle(actives_.begin(), actives_.end(), static_cast<caffe::rng_t*>(prefetch_rng_->generator()));
        }
      }
    }
    unsigned dsz = decoys_.size();
    for (; item_id < batch_size; ++item_id) {
      offind[0] = item_id;
      int offset = batch->data_.offset(offind);
      load_data_from_files(prefetch_data+offset, root_folder, decoys_[decoys_pos_]);
      prefetch_label[item_id] = 1;

      decoys_pos_++;
      if(decoys_pos_ >= dsz) {
        DLOG(INFO) << "Restarting decoys data prefetching from start.";
        decoys_pos_ = 0;
        if (this->layer_param_.ndim_data_param().shuffle()) {
          shuffle(decoys_.begin(), decoys_.end(), static_cast<caffe::rng_t*>(prefetch_rng_->generator()));
        }
      }
    }

  } else {
    //load from all
    unsigned sz = all_.size();
    for (int item_id = 0; item_id < batch_size; ++item_id) {
      offind[0] = item_id;
      int offset = batch->data_.offset(offind);
      load_data_from_files(prefetch_data+offset, root_folder, all_[all_pos_].first);
      prefetch_label[item_id] = all_[all_pos_].second;

      all_pos_++;
      if(all_pos_ >= sz) {
        DLOG(INFO) << "Restarting data prefetching from start.";
        all_pos_ = 0;
        if (this->layer_param_.ndim_data_param().shuffle()) {
          shuffle(all_.begin(), all_.end(), static_cast<caffe::rng_t*>(prefetch_rng_->generator()));
        }
      }
    }
  }


  batch_timer.Stop();
  DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
}

INSTANTIATE_CLASS(NDimDataLayer);
REGISTER_LAYER_CLASS(NDimData);

}  // namespace caffe
