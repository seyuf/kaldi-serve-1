// Server operations.
#pragma once

#include "config.hpp"

// stl includes
#include <iostream>
#include <unordered_map>
#include <memory>
#include <string>
#include <exception>
#include <chrono>

// kaldi includes
#include <base/kaldi-error.h>

// gRPC inludes
#include <grpc/grpc.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

// local includes
#include "decoder.hpp"
#include "kaldi_serve.grpc.pb.h"

void add_alternatives_to_response(const utterance_results_t &results, kaldi_serve::RecognizeResponse *response, const kaldi_serve::RecognitionConfig &config) noexcept {

    kaldi_serve::SpeechRecognitionResult *sr_result = response->add_results();
    kaldi_serve::SpeechRecognitionAlternative *alternative;
    kaldi_serve::Word *word;

    // find alternatives on final `lattice` after all chunks have been processed
    for (auto const &res : results) {
        if (!res.transcript.empty()) {
            alternative = sr_result->add_alternatives();
            alternative->set_transcript(res.transcript);
            alternative->set_confidence(res.confidence);
            alternative->set_am_score(res.am_score);
            alternative->set_lm_score(res.lm_score);
            if (config.word_level()) {
                for (auto const &w: res.words) {
                    word = alternative->add_words();
                    word->set_start_time(w.start_time);
                    word->set_end_time(w.end_time);
                    word->set_word(w.word);
                    word->set_confidence(w.confidence);
                }
            }
        }
    }
}

void add_alternatives_to_responseStream(const utterance_results_t &results, kaldi_serve::StreamingRecognizeResponse *response, const kaldi_serve::RecognitionConfig &config, bool is_final) noexcept {

    kaldi_serve::SpeechRecognitionResult *sr_result = response->add_results();
    kaldi_serve::SpeechRecognitionAlternative *alternative;
    kaldi_serve::Word *word;
    sr_result->set_is_final(is_final);

    // find alternatives on final `lattice` after all chunks have been processed
    for (auto const &res : results) {
        if (!res.transcript.empty()) {
            alternative = sr_result->add_alternatives();
            alternative->set_transcript(res.transcript);
            alternative->set_confidence(res.confidence);
            alternative->set_am_score(res.am_score);
            alternative->set_lm_score(res.lm_score);
            if (config.word_level()) {
                for (auto const &w: res.words) {
                    word = alternative->add_words();
                    word->set_start_time(w.start_time);
                    word->set_end_time(w.end_time);
                    word->set_word(w.word);
                    word->set_confidence(w.confidence);
                }
            }
        }
    }
}

// KaldiServeImpl ::
// Defines the core server logic and request/response handlers.
// Keeps `Decoder` instances cached in a thread-safe
// multiple producer multiple consumer queue to handle each
// request with a separate `Decoder`.
class KaldiServeImpl final : public kaldi_serve::KaldiServe::Service {

  private:
    // Map of Thread-safe Decoder MPMC Queues for diff languages/models
    std::unordered_map<model_id_t, std::unique_ptr<DecoderQueue>, model_id_hash> decoder_queue_map_;

    // Tells if a given model name and language code is available for use.
    inline bool is_model_present(const model_id_t &) const noexcept;

  public:
    explicit KaldiServeImpl(const std::vector<ModelSpec> &) noexcept;

    // Non-Streaming Request Handler RPC service
    // Accepts a single `RecognizeRequest` message
    // Returns a single `RecognizeResponse` message
    grpc::Status Recognize(grpc::ServerContext *const,
                           const kaldi_serve::RecognizeRequest *const,
                           kaldi_serve::RecognizeResponse *const) override;

    // Streaming Request Handler RPC service
    // Accepts a stream of `RecognizeRequest` messages
    // Returns a single `RecognizeResponse` message
    grpc::Status StreamingRecognize(grpc::ServerContext *const,
                                    grpc::ServerReader<kaldi_serve::RecognizeRequest> *const,
                                    kaldi_serve::RecognizeResponse *const) override;

    // Bidirectional Streaming Request Handler RPC service
    // Accepts a stream of `RecognizeRequest` messages
    // Returns a stream of `RecognizeResponse` messages
    grpc::Status BidiStreamingRecognize(grpc::ServerContext *const,
                                        grpc::ServerReaderWriter<kaldi_serve::StreamingRecognizeResponse, kaldi_serve::StreamingRecognizeRequest>*) override;
};

KaldiServeImpl::KaldiServeImpl(const std::vector<ModelSpec> &model_specs) noexcept {
    for (auto const &model_spec : model_specs) {
        model_id_t model_id = std::make_pair(model_spec.name, model_spec.language_code);
        decoder_queue_map_[model_id] = std::make_unique<DecoderQueue>(model_spec);
    }
}

inline bool KaldiServeImpl::is_model_present(const model_id_t &model_id) const noexcept {
    return decoder_queue_map_.find(model_id) != decoder_queue_map_.end();
}

grpc::Status KaldiServeImpl::Recognize(grpc::ServerContext *const context,
                                       const kaldi_serve::RecognizeRequest *const request,
                                       kaldi_serve::RecognizeResponse *const response) {
    const kaldi_serve::RecognitionConfig config = request->config();
    const int32 n_best = config.max_alternatives();
    const std::string model_name = config.model();
    const std::string language_code = config.language_code();
    const model_id_t model_id = std::make_pair(model_name, language_code);

    if (!is_model_present(model_id)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Model " + model_name + " (" + language_code + ") not found");
    }

    // Decoder Acquisition ::
    // - Tries to attain lock and obtain decoder from the queue.
    // - Waits here until lock on queue is attained.
    // - Each new audio stream gets separate decoder object.
    Decoder *decoder_ = decoder_queue_map_[model_id]->acquire();

    std::chrono::system_clock::time_point start_time;
    if (DEBUG) {
        // LOG REQUEST RESOLVE TIME --> START
        start_time = std::chrono::system_clock::now();
    }
    kaldi_serve::RecognitionAudio audio = request->audio();
    std::stringstream input_stream(audio.content());

    utterance_results_t k_results_;

    // decode speech signals in chunks
    try {
        if (config.raw()) {
            decoder_->decode_raw_wav_audio(input_stream, config.data_bytes(), n_best, k_results_, config.word_level());
        } else {
            decoder_->decode_wav_audio(input_stream, n_best, k_results_, config.word_level());
        }
    } catch (kaldi::KaldiFatalError &e) {
        decoder_queue_map_[model_id]->release(decoder_);
        std::string message = std::string(e.what()) + " :: " + std::string(e.KaldiMessage());
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, message);
    } catch (std::exception &e) {
        decoder_queue_map_[model_id]->release(decoder_);
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }

    add_alternatives_to_response(k_results_, response, config);

    // Decoder Release ::
    // - Releases the lock on the decoder and pushes back into queue.
    // - Notifies another request handler thread of availability.
    decoder_queue_map_[model_id]->release(decoder_);

    if (DEBUG) {
        std::chrono::system_clock::time_point end_time = std::chrono::system_clock::now();
        // LOG REQUEST RESOLVE TIME --> END
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        std::cout << "request resolved in: " << ms.count() << "ms" << ENDL;
    }

    return grpc::Status::OK;
}

grpc::Status KaldiServeImpl::StreamingRecognize(grpc::ServerContext *const context,
                                                grpc::ServerReader<kaldi_serve::RecognizeRequest> *const reader,
                                                kaldi_serve::RecognizeResponse *const response) {
    kaldi_serve::RecognizeRequest request_;
    reader->Read(&request_);

    // We first read the request to see if we have the correct model and language to load
    // Assuming: config may change mid-way (only `raw` and `data_bytes` fields)
    kaldi_serve::RecognitionConfig config = request_.config();
    const int32 n_best = config.max_alternatives();
    const std::string model_name = config.model();
    const std::string language_code = config.language_code();
    const model_id_t model_id = std::make_pair(model_name, language_code);

    if (!is_model_present(model_id)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Model " + model_name + " (" + language_code + ") not found");
    }

    // Decoder Acquisition ::
    // - Tries to attain lock and obtain decoder from the queue.
    // - Waits here until lock on queue is attained.
    // - Each new audio stream gets separate decoder object.
    Decoder *decoder_ = decoder_queue_map_[model_id]->acquire();

    // decoder state variables need to be statically initialized
    kaldi::OnlineIvectorExtractorAdaptationState adaptation_state(decoder_->feature_info_->ivector_extractor_info);
    kaldi::OnlineNnet2FeaturePipeline feature_pipeline(*decoder_->feature_info_);
    feature_pipeline.SetAdaptationState(adaptation_state);

    kaldi::OnlineSilenceWeighting silence_weighting(decoder_->trans_model_, decoder_->feature_info_->silence_weighting_config,
                                                    decoder_->decodable_opts_.frame_subsampling_factor);
    kaldi::SingleUtteranceNnet3Decoder decoder(decoder_->lattice_faster_decoder_config_,
                                               decoder_->trans_model_, *decoder_->decodable_info_.get(), *decoder_->decode_fst_,
                                               &feature_pipeline);

    std::chrono::system_clock::time_point start_time;

    // read chunks until end of stream
    do {
        if (DEBUG) {
            // LOG REQUEST RESOLVE TIME --> START (at the last request since that would be the actual latency)
            start_time = std::chrono::system_clock::now();
        }
        config = request_.config();
        kaldi_serve::RecognitionAudio audio = request_.audio();
        std::stringstream input_stream_chunk(audio.content());

        // decode intermediate speech signals
        // Assuming: audio stream has already been chunked into desired length
        try {
            if (config.raw()) {
                decoder_->decode_stream_raw_wav_chunk(feature_pipeline, silence_weighting, decoder, input_stream_chunk, config.data_bytes());
            } else {
                decoder_->decode_stream_wav_chunk(feature_pipeline, silence_weighting, decoder, input_stream_chunk);
            }
        } catch (kaldi::KaldiFatalError &e) {
            decoder_queue_map_[model_id]->release(decoder_);
            std::string message = std::string(e.what()) + " :: " + std::string(e.KaldiMessage());
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, message);
        } catch (std::exception &e) {
            decoder_queue_map_[model_id]->release(decoder_);
            return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
        }
    } while (reader->Read(&request_));

    utterance_results_t k_results_;
    decoder_->decode_stream_final(feature_pipeline, decoder, n_best, k_results_, config.word_level());

    add_alternatives_to_response(k_results_, response, config);

    // Decoder Release ::
    // - Releases the lock on the decoder and pushes back into queue.
    // - Notifies another request handler thread of availability.
    decoder_queue_map_[model_id]->release(decoder_);

    if (DEBUG) {
        std::chrono::system_clock::time_point end_time = std::chrono::system_clock::now();
        // LOG REQUEST RESOLVE TIME --> END
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        std::cout << "request resolved in: " << ms.count() << "ms" << ENDL;
    }

    return grpc::Status::OK;
}

grpc::Status KaldiServeImpl::BidiStreamingRecognize(grpc::ServerContext *const context,
                                                    grpc::ServerReaderWriter<kaldi_serve::StreamingRecognizeResponse, kaldi_serve::StreamingRecognizeRequest> *stream) {
    kaldi_serve::StreamingRecognizeRequest request_;
    stream->Read(&request_);

    // We first read the request to see if we have the correct model and language to load
    // Assuming: config may change mid-way (only `raw` and `data_bytes` fields)
    kaldi_serve::RecognitionConfig config = request_.config();
    const int32 n_best = config.max_alternatives();
    const std::string model_name = config.model();
    const std::string language_code = config.language_code();
    const model_id_t model_id = std::make_pair(model_name, language_code);

    if (!is_model_present(model_id)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Model " + model_name + " (" + language_code + ") not found");
    }

    // Decoder Acquisition ::
    // - Tries to attain lock and obtain decoder from the queue.
    // - Waits here until lock on queue is attained.
    // - Each new audio stream gets separate decoder object.
    Decoder *decoder_ = decoder_queue_map_[model_id]->acquire();

    // decoder state variables need to be statically initialized
    kaldi::OnlineIvectorExtractorAdaptationState adaptation_state(decoder_->feature_info_->ivector_extractor_info);
    kaldi::OnlineNnet2FeaturePipeline feature_pipeline(*decoder_->feature_info_);
    feature_pipeline.SetAdaptationState(adaptation_state);

    kaldi::OnlineSilenceWeighting silence_weighting(decoder_->trans_model_, decoder_->feature_info_->silence_weighting_config,
                                                    decoder_->decodable_opts_.frame_subsampling_factor);
    kaldi::SingleUtteranceNnet3Decoder decoder(decoder_->lattice_faster_decoder_config_,
                                               decoder_->trans_model_, *decoder_->decodable_info_.get(), *decoder_->decode_fst_,
                                               &feature_pipeline);

    std::chrono::system_clock::time_point start_time;

    // read chunks until end of stream
    do {



        if (DEBUG) {
            // LOG REQUEST RESOLVE TIME --> START (at the last request since that would be the actual latency)
            start_time = std::chrono::system_clock::now();
        }
        config = request_.config();
        kaldi_serve::RecognitionAudio audio = request_.audio();
        std::stringstream input_stream_chunk(audio.content());

        // decode intermediate speech signals
        // Assuming: audio stream has already been chunked into desired length
        try {
            if (config.raw()) {
                decoder_->decode_stream_raw_wav_chunk(feature_pipeline, silence_weighting, decoder, input_stream_chunk, config.data_bytes());
            } else {
                decoder_->decode_stream_wav_chunk(feature_pipeline, silence_weighting, decoder, input_stream_chunk);
            }

            utterance_results_t k_results_;
            decoder_->decode_stream_final(feature_pipeline, decoder, n_best, k_results_, config.word_level(), true);

            kaldi_serve::StreamingRecognizeResponse response_;
            add_alternatives_to_responseStream(k_results_, &response_, config, false);

            stream->Write(response_);

        } catch (kaldi::KaldiFatalError &e) {
            decoder_queue_map_[model_id]->release(decoder_);
            std::string message = std::string(e.what()) + " :: " + std::string(e.KaldiMessage());
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, message);
        } catch (std::exception &e) {
            decoder_queue_map_[model_id]->release(decoder_);
            return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
        }
    } while (stream->Read(&request_) && !request_.config().end_of_utterance());

    utterance_results_t k_results_;
    decoder_->decode_stream_final(feature_pipeline, decoder, n_best, k_results_, config.word_level());

    kaldi_serve::StreamingRecognizeResponse response_;
    
    add_alternatives_to_responseStream(k_results_, &response_, config, true);

    stream->Write(response_);
    // writer->

    // Decoder Release ::
    // - Releases the lock on the decoder and pushes back into queue.
    // - Notifies another request handler thread of availability.
    decoder_queue_map_[model_id]->release(decoder_);

    if (DEBUG) {
        std::chrono::system_clock::time_point end_time = std::chrono::system_clock::now();
        // LOG REQUEST RESOLVE TIME --> END
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        std::cout << "request resolved in: " << ms.count() << "ms" << ENDL;
    }

    return grpc::Status::OK;
}

// Runs the Server with the Kaldi Service
void run_server(const std::vector<ModelSpec> &model_specs) {
    KaldiServeImpl service(model_specs);

    std::string server_address("0.0.0.0:5016");

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());

    std::cout << "kaldi-serve gRPC Streaming Server listening on " << server_address << ENDL;
    server->Wait();
}

/**
NOTES:
------

VARIABLES ON WHICH SERVER RELIABILITY DEPENDS ::
    1. Length of Audio Stream (in secs)
    2. No. of chunks in the Audio Stream
    3. Time intervals between subsequent chunks of audio stream
    4. No. of Decoders in Queue
    5. Timeout for each request (chunk essentially)
    6. No. of concurrent streams being handled by the server
 */
