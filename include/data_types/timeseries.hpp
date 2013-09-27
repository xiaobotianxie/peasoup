#pragma once
#include <vector>
#include "cuda.h"
#include "cufft.h"
#include <thrust/copy.h>
#include <thrust/device_ptr.h>
#include "utils/exceptions.hpp"
#include "utils/utils.hpp"
#include <data_types/header.hpp>
#include <string>
#include "kernels/kernels.h"
#include "kernels/defaults.h"

//TEMP
#include <stdio.h>
#include <iostream>

//######################
template <class T> class TimeSeries {
protected:
  T* data_ptr;
  unsigned int nsamps;
  float tsamp;
  
public:  
  TimeSeries(T* data_ptr,unsigned int nsamps,float tsamp)
    :data_ptr(data_ptr), nsamps(nsamps), tsamp(tsamp){}

  TimeSeries(void)
    :data_ptr(0), nsamps(0.0), tsamp(0.0) {}

  TimeSeries(unsigned int nsamps)
    :data_ptr(0), nsamps(nsamps), tsamp(0.0){}
  
  T operator[](int idx){
    return data_ptr[idx];
  }
  
  T* get_data(void){return data_ptr;}
  void set_data(T* data_ptr){this->data_ptr = data_ptr;};
  unsigned int get_nsamps(void){return nsamps;}
  void set_nsamps(unsigned int nsamps){this->nsamps = nsamps;}
  float get_tsamp(void){return tsamp;}
  void set_tsamp(float tsamp){this->tsamp = tsamp;}
  
  virtual void from_file(std::string filename)
  {
    std::ifstream infile;
    SigprocHeader hdr;
    infile.open(filename.c_str(),std::ifstream::in | std::ifstream::binary);
    ErrorChecker::check_file_error(infile, filename);
    read_header(infile,hdr);
    if (hdr.nbits/8!=sizeof(T))
      ErrorChecker::throw_error("Bad bit size in input time series");
    size_t input_size = (size_t) hdr.nsamples*sizeof(T);
    this->data_ptr = new T [hdr.nsamples];
    infile.seekg(hdr.size, std::ios::beg);
    infile.read(reinterpret_cast<char*>(this->data_ptr), input_size);
    this->nsamps = hdr.nsamples;
    this->tsamp = hdr.tsamp;
  }
};




//#########################

template <class T>
class DedispersedTimeSeries: public TimeSeries<T> {
private:
  float dm;

public:
  DedispersedTimeSeries()
    :TimeSeries<T>(),dm(0.0){}

  DedispersedTimeSeries(T* data_ptr, unsigned int nsamps, float tsamp, float dm)
    :TimeSeries<T>(data_ptr,nsamps,tsamp),dm(dm){}
  
  float get_dm(void){return dm;}
  void set_dm(float dm){this->dm = dm;}
};

//###########################

template <class T>
class FilterbankChannel: public TimeSeries<T> {
private:
  float freq;
  
public:
  FilterbankChannel(T* data_ptr, unsigned int nsamps, float tsamp, float freq)
    :TimeSeries<T>(data_ptr,nsamps,tsamp),freq(freq){}
};


template <class OnDeviceType>
class DeviceTimeSeries: public TimeSeries<OnDeviceType> {
public:
  DeviceTimeSeries(unsigned int nsamps)
    :TimeSeries<OnDeviceType>(nsamps)
  {
    Utils::device_malloc<OnDeviceType>(&this->data_ptr,nsamps);
  }

  template <class OnHostType>
  DeviceTimeSeries(TimeSeries<OnHostType>& host_tim)
    :TimeSeries<OnDeviceType>(host_tim.get_nsamps())
  {
    OnHostType* copy_buffer;
    Utils::device_malloc<OnDeviceType>(&this->data_ptr,this->nsamps);
    Utils::device_malloc<OnHostType>(&copy_buffer,this->nsamps);
    Utils::h2dcpy(copy_buffer,host_tim.get_data(),this->nsamps*sizeof(OnHostType));
    device_conversion<OnHostType,OnDeviceType>(copy_buffer, this->data_ptr,
                                               (unsigned int)this->nsamps,
                                               (unsigned int)MAX_BLOCKS,
                                               (unsigned int)MAX_THREADS);
    this->tsamp = host_tim.get_tsamp();
    Utils::device_free(copy_buffer);
  }

  void fill(size_t start, size_t end, OnDeviceType value){
    if (end > this->nsamps)
      ErrorChecker::throw_error("DeviceTimeSeries::fill bad end value requested");
    GPU_fill(this->data_ptr+start,this->data_ptr+end,value);
  }
 
  ~DeviceTimeSeries()
  {
    Utils::device_free(this->data_ptr);
  }

};


template <class OnDeviceType,class OnHostType>
class ReusableDeviceTimeSeries: public DeviceTimeSeries<OnDeviceType> {
private:
  OnHostType* copy_buffer;
  
public:
  ReusableDeviceTimeSeries(unsigned int nsamps)
    :DeviceTimeSeries<OnDeviceType>(nsamps)
  {
    Utils::device_malloc<OnHostType>(&copy_buffer,this->nsamps);
  }
  
  void copy_from_host(TimeSeries<OnHostType>& host_tim)
  {
    size_t size = std::min(host_tim.get_nsamps(),this->nsamps);
    this->tsamp = host_tim.get_tsamp();
    Utils::h2dcpy(copy_buffer, host_tim.get_data(), size*sizeof(OnHostType));
    device_conversion<OnHostType,OnDeviceType>(copy_buffer, this->data_ptr,
                                               (unsigned int)size,
                                               (unsigned int)MAX_BLOCKS,
					       (unsigned int)MAX_THREADS);
    
  }

  ~ReusableDeviceTimeSeries()
  {
    Utils::device_free(copy_buffer);
  }
};

  
//#############################

template <class T>
class TimeSeriesContainer {
protected:
  T* data_ptr;
  unsigned int nsamps;
  float tsamp;
  unsigned int count;
  
  TimeSeriesContainer(T* data_ptr, unsigned int nsamps, float tsamp, unsigned int count)
    :data_ptr(data_ptr),nsamps(nsamps),tsamp(tsamp),count(count){}
  
public:
  unsigned int get_count(void){return count;}
  unsigned int get_nsamps(void){return nsamps;}
  void set_tsamp(float tsamp){this->tsamp = tsamp;}
  float get_tsamp(void){return tsamp;}
  T* get_data(void){return data_ptr;}
};

//created through Dedisperser
template <class T>
class DispersionTrials: public TimeSeriesContainer<T> {
private:
  std::vector<float> dm_list;
  
public:
  DispersionTrials(T* data_ptr, unsigned int nsamps, float tsamp, std::vector<float> dm_list_in)
    :TimeSeriesContainer<T>(data_ptr,nsamps,tsamp, (unsigned int)dm_list_in.size())
  {
    dm_list.swap(dm_list_in);
  }
  
  DedispersedTimeSeries<T> operator[](int idx)
  {
    T* ptr = this->data_ptr+idx*(size_t)this->nsamps;
    return DedispersedTimeSeries<T>(ptr, this->nsamps, this->tsamp, dm_list[idx]);
  }
  
  void get_idx(unsigned int idx, DedispersedTimeSeries<T>& tim){
    T* ptr = this->data_ptr+(size_t)idx*(size_t)this->nsamps;
    tim.set_data(ptr);
    tim.set_dm(dm_list[idx]);
    tim.set_nsamps(this->nsamps);
    tim.set_tsamp(this->tsamp);
  }
};



//created through Channeliser
template <class T>
class FilterbankChannels: public TimeSeriesContainer<T> {

public:
  FilterbankChannel<T> operator[](int idx);
  FilterbankChannel<T> nearest_chan(float freq);
  
};
