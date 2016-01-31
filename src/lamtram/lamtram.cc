#include <lamtram/lamtram.h>
#include <lamtram/macros.h>
#include <lamtram/vocabulary.h>
#include <lamtram/sentence.h>
#include <lamtram/timer.h>
#include <lamtram/macros.h>
#include <lamtram/neural-lm.h>
#include <lamtram/encoder-decoder.h>
#include <lamtram/encoder-attentional.h>
#include <lamtram/encoder-classifier.h>
#include <lamtram/model-utils.h>
#include <lamtram/string-util.h>
#include <lamtram/ensemble-decoder.h>
#include <lamtram/ensemble-classifier.h>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <iostream>
#include <fstream>
#include <string>

using namespace std;
using namespace lamtram;
namespace po = boost::program_options;

typedef std::unordered_map<std::string, std::pair<std::string, float> > Mapping;
typedef std::vector<std::vector<float> > Sentence;

void Lamtram::MapWords(const vector<string> & src_strs, const Sentence & trg_sent, const Sentence & align, const Mapping & mapping, int pad, vector<string> & trg_strs) {
  if(align.size() == 0) return;
  assert(trg_sent.size() >= trg_strs.size() + pad);
  assert(align.size() == trg_sent.size());
  WordId unk_id = 1;
  for(size_t i = 0; i < trg_strs.size(); i++) {
    if(trg_sent[i+pad] == unk_id) {
      size_t max_id = align[i];
      if(max_id != -1) {
        assert(src_strs.size() > max_id);
        auto it = mapping.find(src_strs[max_id]);
        trg_strs[i] = (it != mapping.end()) ? it->second.first : src_strs[max_id];
      }
    }
  }
}

int Lamtram::SequenceOperation(const boost::program_options::variables_map & vm) {
  // Models
  vector<NeuralLMPtr> lms;
  vector<EncoderDecoderPtr> encdecs;
  vector<EncoderAttentionalPtr> encatts;
  vector<shared_ptr<cnn::Model> > models;
  VocabularyPtr vocab_src, vocab_trg;

  int max_minibatch_size = vm["minibatch_size"].as<int>();
  
  // Buffers
  string line;
  vector<string> strs;

  // Read in the files
  int pad = 0;
  vector<string> infiles;
  boost::split(infiles, vm["models_in"].as<std::string>(), boost::is_any_of("|"));
  string type, file;
  for(string & infile : infiles) {
    int eqpos = infile.find('=');
    if(eqpos == string::npos)
      THROW_ERROR("Bad model type. Must specify encdec=, encatt=, or nlm= before model name." << endl << infile);
    type = infile.substr(0, eqpos);
    file = infile.substr(eqpos+1);
    VocabularyPtr vocab_src_temp, vocab_trg_temp;
    shared_ptr<cnn::Model> mod_temp;
    // Read in the model
    if(type == "encdec") {
      EncoderDecoder * tm = ModelUtils::LoadModel<EncoderDecoder>(file, mod_temp, vocab_src_temp, vocab_trg_temp);
      encdecs.push_back(shared_ptr<EncoderDecoder>(tm));
      pad = max(pad, tm->GetDecoder().GetNgramContext());
    } else if(type == "encatt") {
      EncoderAttentional * tm = ModelUtils::LoadModel<EncoderAttentional>(file, mod_temp, vocab_src_temp, vocab_trg_temp);
      encatts.push_back(shared_ptr<EncoderAttentional>(tm));
      pad = max(pad, tm->GetDecoder().GetNgramContext());
    } else if(type == "nlm") {
      NeuralLM * lm = ModelUtils::LoadModel<NeuralLM>(file, mod_temp, vocab_src_temp, vocab_trg_temp);
      lms.push_back(shared_ptr<NeuralLM>(lm));
      pad = max(pad, lm->GetNgramContext());
    }
    // Sanity check
    if(vocab_trg.get() && *vocab_trg_temp != *vocab_trg)
      THROW_ERROR("Target vocabularies for translation/language models are not equal.");
    if(vocab_src.get() && vocab_src_temp.get() && *vocab_src_temp != *vocab_src)
      THROW_ERROR("Source vocabularies for translation/language models are not equal.");
    models.push_back(mod_temp);
    vocab_trg = vocab_trg_temp;
    if(vocab_src_temp.get()) vocab_src = vocab_src_temp;
  }
  int vocab_size = vocab_trg->size();

  // Get the mapping table if necessary
  Mapping mapping;
  if(vm["map_in"].as<std::string>() != "") {
    ifstream map_in(vm["map_in"].as<std::string>());
    if(!map_in)
      THROW_ERROR("Could not find map_in file " << vm["map_in"].as<std::string>());
    while(getline(map_in, line)) {
      boost::split(strs, line, boost::is_any_of("\t"));
      if(strs.size() != 3)
        THROW_ERROR("Invalid line in mapping file: " << line);
      float my_score = stof(strs[2]);
      auto it = mapping.find(strs[0]);
      if(it == mapping.end() || it->second.second < my_score)
        mapping[strs[0]] = make_pair(strs[1], my_score);
    }
  }

  // Get the source input if necessary
  shared_ptr<ifstream> src_in;
  if(encdecs.size() + encatts.size() > 0) {
    src_in.reset(new ifstream(vm["src_in"].as<std::string>()));
    if(!*src_in)
      THROW_ERROR("Could not find src_in file " << vm["src_in"].as<std::string>());
  }
  
  // Find the range
  pair<size_t,size_t> sent_range(0,INT_MAX);
  if(vm["sent_range"].as<string>() != "") {
    std::vector<string> range_str = Tokenize(vm["sent_range"].as<string>(), ",");
    if(range_str.size() != 2)
      THROW_ERROR("When specifying a range must be two comma-delimited numbers, but got: " << vm["sent_range"].as<string>());
    sent_range.first = std::stoi(range_str[0]);
    sent_range.second = std::stoi(range_str[1]);
  }
  
  // Create the decoder
  EnsembleDecoder decoder(encdecs, encatts, lms, pad);
  decoder.SetWordPen(vm["word_pen"].as<float>());
  decoder.SetEnsembleOperation(vm["ensemble_op"].as<string>());
  decoder.SetBeamSize(vm["beam"].as<int>());
  decoder.SetSizeLimit(vm["size_limit"].as<int>());

  
  // Perform operation
  string operation = vm["operation"].as<std::string>();
  Sentence sent_src, sent_trg;
  vector<string> str_src, str_trg;
  Sentence align;
  int last_id = -1;
  bool do_sent = false;
  if(operation == "ppl") {
    LLStats corpus_ll(vocab_size);
    Timer time;
    while(getline(cin, line)) { 
      // Get the target, and if it exists, source sentences
      if(GlobalVars::verbose >= 2) { cerr << "SentLL trg: " << line << endl; }
      sent_trg = vocab_trg->ParseWords(line, pad, true);
      if(encdecs.size() + encatts.size() > 0) {
        if(!getline(*src_in, line))
          THROW_ERROR("Source and target files don't match");
        if(GlobalVars::verbose >= 2) { cerr << "SentLL src: " << line << endl; }
        sent_src = vocab_src->ParseWords(line, 0, false);
      }
      last_id++;
      // If we're inside the range, do it
      if(last_id >= sent_range.first && last_id < sent_range.second) {
        LLStats sent_ll(vocab_size);
        decoder.CalcSentLL<Sentence,LLStats>(sent_src, sent_trg, sent_ll);
        if(GlobalVars::verbose >= 1) { cout << "ll=" << sent_ll.CalcUnkLik() << " unk=" << sent_ll.unk_  << endl; }
        corpus_ll += sent_ll;
      }
    }
    double elapsed = time.Elapsed();
    cerr << "ppl=" << corpus_ll.CalcPPL() << ", unk=" << corpus_ll.unk_ << ", time=" << elapsed << " (" << corpus_ll.words_/elapsed << " w/s)" << endl;
  } else if(operation == "nbest") {
    Timer time;
    int all_words = 0, curr_words = 0;
    std::vector<Sentence> sents_trg;
    while(getline(cin, line)) { 
      // Get the new sentence
      vector<string> columns = Tokenize(line, " ||| ");
      if(columns.size() < 2) THROW_ERROR("Bad line in n-best:\n" << line);
      int my_id = stoi(columns[0]);
      sent_trg = vocab_trg->ParseWords(columns[1], pad, true);
      // If we've finished the current source, print
      if((my_id != last_id || curr_words+sents_trg.size() > max_minibatch_size) && sents_trg.size() > 0) {
        vector<LLStats> sents_ll(sents_trg.size(), LLStats(vocab_size));
        if(sents_trg.size() > 1)
          decoder.CalcSentLL<vector<Sentence>,vector<LLStats> >(sent_src, sents_trg, sents_ll);
        else
          decoder.CalcSentLL<Sentence,LLStats>(sent_src, sents_trg[0], sents_ll[0]);
        for(auto & sent_ll : sents_ll)
          cout << "ll=" << sent_ll.CalcUnkLik() << " unk=" << sent_ll.unk_  << endl;
        sents_trg.resize(0);
        curr_words = 0;
      }
      // Load the new source word
      if(my_id != last_id) {
        if(!getline(*src_in, line))
          THROW_ERROR("Source and target files don't match");
        sent_src = vocab_src->ParseWords(line, 0, false);
        if(do_sent) {
          double elapsed = time.Elapsed();
          cerr << "sent=" << last_id << ", time=" << elapsed << " (" << all_words/elapsed << " w/s)" << endl;
        }
        last_id = my_id;
        do_sent = (last_id >= sent_range.first && last_id < sent_range.second);
      }
      // Add to the data
      if(do_sent) {
        sents_trg.push_back(sent_trg);
        all_words += sent_trg.size()-pad;
        curr_words += sent_trg.size()-pad;
      }
    }
    if(do_sent) {
      vector<LLStats> sents_ll(sents_trg.size(), LLStats(vocab_size));
      decoder.CalcSentLL<vector<Sentence>,vector<LLStats> >(sent_src, sents_trg, sents_ll);
      for(auto & sent_ll : sents_ll)
        cout << "ll=" << sent_ll.CalcUnkLik() << " unk=" << sent_ll.unk_  << endl;
      double elapsed = time.Elapsed();
      cerr << "sent=" << last_id << ", time=" << elapsed << " (" << all_words/elapsed << " w/s)" << endl;
    }
  } else if(operation == "gen" || operation == "samp") {
    if(operation == "samp") THROW_ERROR("Sampling not implemented yet");
    for(int i = 0; i < sent_range.second; ++i) {
      if(encdecs.size() + encatts.size() > 0) {
        if(!getline(*src_in, line)) break;
        str_src = vocab_src->SplitWords(line);
        sent_src = vocab_src->ParseWords(str_src, 0, false);
      }
      if(i >= sent_range.first) {
        sent_trg = decoder.Generate(sent_src, align);
        str_trg = vocab_trg->ConvertWords(sent_trg, false);
        MapWords(str_src, sent_trg, align, mapping, pad, str_trg);
        cout << vocab_trg->PrintWords(str_trg) << endl;
      }
    }
  } else {
    THROW_ERROR("Illegal operation " << operation);
  }

  return 0;
}

int Lamtram::ClassifierOperation(const boost::program_options::variables_map & vm) {
  // Models
  vector<EncoderClassifierPtr> encclss;
  shared_ptr<Vocabulary> vocab_src, vocab_trg;
  vector<shared_ptr<cnn::Model> > models;

  // Read in the files
  vector<string> infiles;
  boost::split(infiles, vm["models_in"].as<std::string>(), boost::is_any_of("|"));
  string type, file;
  for(string & infile : infiles) {
    int eqpos = infile.find('=');
    if(eqpos == string::npos)
      THROW_ERROR("Bad model type. Must specify enccls= before model name." << endl << infile);
    type = infile.substr(0, eqpos);
    if(type != "enccls")
      THROW_ERROR("Bad model type. Must specify enccls= before model name." << endl << infile);
    file = infile.substr(eqpos+1);
    VocabularyPtr vocab_src_temp, vocab_trg_temp;
    shared_ptr<cnn::Model> mod_temp;
    // Read in the model
    EncoderClassifier * tm = ModelUtils::LoadModel<EncoderClassifier>(file, mod_temp, vocab_src_temp, vocab_trg_temp);
    encclss.push_back(shared_ptr<EncoderClassifier>(tm));
    // Sanity check
    if(vocab_trg.get() && *vocab_trg_temp != *vocab_trg)
      THROW_ERROR("Target vocabularies for translation/language models are not equal.");
    if(vocab_src.get() && vocab_src_temp.get() && *vocab_src_temp != *vocab_src)
      THROW_ERROR("Target vocabularies for translation/language models are not equal.");
    models.push_back(mod_temp);
    vocab_trg = vocab_trg_temp;
    vocab_src = vocab_src_temp;
  }
  int vocab_size = vocab_trg->size();

  // Get the source input
  shared_ptr<ifstream> src_in;
  src_in.reset(new ifstream(vm["src_in"].as<std::string>()));
  if(!*src_in)
    THROW_ERROR("Could not find src_in file " << vm["src_in"].as<std::string>());
  
  // Create the decoder
  EnsembleClassifier ensemble(encclss);
  ensemble.SetEnsembleOperation(vm["ensemble_op"].as<string>());
  
  // Perform operation
  string operation = vm["operation"].as<std::string>();
  string line;
  Sentence sent_src;
  int trg;
  if(operation == "clseval") {
    LLStats corpus_ll(vocab_size);
    Timer time;
    while(getline(cin, line)) { 
      LLStats sent_ll(vocab_size);
      // Get the target, and if it exists, source sentences
      if(GlobalVars::verbose > 0) { cerr << "ClsEval trg: " << line << endl; }
      trg = vocab_trg->WID(line);
      if(!getline(*src_in, line))
        THROW_ERROR("Source and target files don't match");
      if(GlobalVars::verbose > 0) { cerr << "ClsEval src: " << line << endl; }
      sent_src = vocab_src->ParseWords(line, 0, false);
      // If the encoder
      ensemble.CalcEval(sent_src, trg, sent_ll);
      if(GlobalVars::verbose > 0) { cout << "ll=" << sent_ll.CalcUnkLik() << " correct=" << sent_ll.correct_ << endl; }
      corpus_ll += sent_ll;
    }
    double elapsed = time.Elapsed();
    cerr << "ppl=" << corpus_ll.CalcPPL() << ", acc="<< corpus_ll.CalcAcc() << ", time=" << elapsed << " (" << corpus_ll.words_/elapsed << " w/s)" << endl;
  } else if(operation == "cls") {
    while(getline(*src_in, line)) {
      sent_src = vocab_src->ParseWords(line, 0, false);
      trg = ensemble.Predict(sent_src);
      cout << vocab_trg->WSym(trg) << endl;
    }
  } else {
    THROW_ERROR("Illegal operation " << operation);
  }

  return 0;
}

int Lamtram::main(int argc, char** argv) {
  po::options_description desc("*** lamtram-train (by Graham Neubig) ***");
  desc.add_options()
    ("help", "Produce help message")
    ("verbose", po::value<int>()->default_value(0), "How much verbose output to print")
    ("beam", po::value<int>()->default_value(1), "Number of hypotheses to expand")
    ("cnn_mem", po::value<int>()->default_value(512), "How much memory to allocate to cnn")
    ("ensemble_op", po::value<string>()->default_value("sum"), "The operation to use when ensembling probabilities (sum/logsum)")
    ("map_in", po::value<string>()->default_value(""), "A file containing a mapping table (\"src trg prob\" format)")
    ("minibatch_size", po::value<int>()->default_value(1), "Max size of a minibatch in words (may be exceeded if there are longer sentences)")
    ("models_in", po::value<string>()->default_value(""), "Model files in format \"{encdec,encatt,nlm}=filename\" with encdec for encoder-decoders, encatt for attentional models, nlm for language models. When multiple, separate by a pipe.")
    ("operation", po::value<string>()->default_value("ppl"), "Operations (ppl: measure perplexity, nbest: score n-best list, gen: generate most likely sentence, samp: sample sentences randomly)")
    ("sent_range", po::value<string>()->default_value(""), "Optionally specify a comma-delimited range on how many sentences to process")
    ("size_limit", po::value<int>()->default_value(2000), "Limit on the size of sentences")
    ("src_in", po::value<string>()->default_value(""), "File to read the source from, if any")
    ("word_pen", po::value<float>()->default_value(0.0), "The \"word penalty\", a larger value favors longer sentences, shorter favors shorter")
    ;
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);   
  if (vm.count("help")) {
    cout << desc << endl;
    return 1;
  }
  for(int i = 0; i < argc; i++) { cerr << argv[i] << " "; } cerr << endl;

  GlobalVars::verbose = vm["verbose"].as<int>();

  string operation = vm["operation"].as<std::string>();
  if(operation == "ppl" || operation == "nbest" || operation == "gen" || operation == "samp") {
    return SequenceOperation(vm);
  } else if(operation == "cls" || operation == "clseval") {
    return ClassifierOperation(vm);
  } else {
    THROW_ERROR("Illegal operation: " << operation);
  }

}
