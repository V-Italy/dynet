#include "dynet/nodes.h"
#include "dynet/dynet.h"
#include "dynet/training.h"
#include "dynet/timing.h"
#include "dynet/rnn.h"
#include "dynet/gru.h"
#include "dynet/lstm.h"
#include "dynet/dict.h"
#include "dynet/expr.h"
#include "dynet/globals.h"
#include "dynet/io.h"
#include "../utils/getpid.h"

#include <iostream>
#include <fstream>
#include <sstream>

using namespace std;
using namespace dynet;

unsigned LAYERS = 2;
unsigned INPUT_DIM = 32;  //256
unsigned HIDDEN_DIM = 128;  // 1024
unsigned HIDDEN_DIM2 = 32;
unsigned VOCAB_SIZE = 0;
unsigned LATENT_DIM = 2;
unsigned L = 10;
vector<vector<float>> eps(L, vector<float>(LATENT_DIM));

dynet::Dict d;
int kSOS;
int kEOS;

template <class Builder>
struct RNNLanguageModel {
  LookupParameter p_c;  // should we have two of these?
  Builder ebuilder;
  Parameter p_H;
  Parameter p_hb;
  Parameter p_h2m;
  Parameter p_mb;
  Parameter p_h2s;
  Parameter p_sb;

  // DECODER
  Builder dbuilder;
  Parameter p_R;
  Parameter p_bias;
  Parameter p_z2h0;
  Parameter p_h0b;

  explicit RNNLanguageModel(ParameterCollection& model) :
      ebuilder(LAYERS, INPUT_DIM, HIDDEN_DIM, model),
      dbuilder(LAYERS, INPUT_DIM, HIDDEN_DIM, model) {
    p_H = model.add_parameters({HIDDEN_DIM2, HIDDEN_DIM});
    p_hb = model.add_parameters({HIDDEN_DIM2});
    p_h2m = model.add_parameters({LATENT_DIM, HIDDEN_DIM2});
    p_mb = model.add_parameters({LATENT_DIM});
    p_h2s = model.add_parameters({LATENT_DIM, HIDDEN_DIM2});
    p_sb = model.add_parameters({LATENT_DIM});

    p_z2h0 = model.add_parameters({HIDDEN_DIM * LAYERS, LATENT_DIM});
    p_h0b = model.add_parameters({HIDDEN_DIM * LAYERS});

    p_c = model.add_lookup_parameters(VOCAB_SIZE, {INPUT_DIM}); 
    p_R = model.add_parameters({VOCAB_SIZE, HIDDEN_DIM});
    p_bias = model.add_parameters({VOCAB_SIZE});
  }

  // return Expression of total loss
  Expression BuildLMGraph(const vector<int>& sent, ComputationGraph& cg, bool flag = false) {
    const unsigned slen = sent.size() - 1;
    ebuilder.new_graph(cg);  // reset RNN builder for new graph

    ebuilder.start_new_sequence();
    Expression i_R = parameter(cg, p_R); // hidden -> word rep parameter
    Expression i_bias = parameter(cg, p_bias);  // word bias
    for (unsigned t = 0; t < slen; ++t) {
      Expression i_x_t = lookup(cg, p_c, sent[t]);
      // y_t = RNN(x_t)
      ebuilder.add_input(i_x_t);
    }
    Expression i_H = parameter(cg, p_H);
    Expression i_hb = parameter(cg, p_hb);
    Expression h = tanh(i_H * ebuilder.back() + i_hb);
    Expression mu = parameter(cg, p_h2m) * h + parameter(cg, p_mb);
    if (flag) {
      vector<float> v = as_vector(cg.get_value(mu.i));
      for (unsigned i = 0; i < (slen-2); ++i) cout << d.convert(sent[i+1]);
      cout << " |||";
      for (auto& x : v) cout << ' ' << x;
      cout << endl;
    }
    Expression logsig = 0.5 * parameter(cg, p_h2s) * h + parameter(cg, p_sb);
    Expression log_prior = 0.5 * sum_cols(transpose(2 * logsig - square(mu) - exp(2 * logsig)));
    vector<Expression> errs;
    dbuilder.new_graph(cg);
    for (unsigned l = 0; l < L; ++l) { // noise samples
      for (auto& x : eps[l]) x = rand_normal();
      Expression ceps = input(cg, {LATENT_DIM}, &eps[l]);
      Expression z = mu + cmult(ceps, exp(logsig));
      Expression h0 = parameter(cg, p_z2h0) * z + parameter(cg, p_h0b);
      vector<Expression> h0s(LAYERS);
      for (unsigned i = 0; i < LAYERS; ++i) {
        h0s[i] = pickrange(h0, i * HIDDEN_DIM, (i+1) * HIDDEN_DIM);
      }
      dbuilder.start_new_sequence(h0s);
      for (unsigned t = 0; t < slen; ++t) {
        Expression i_x_t = lookup(cg, p_c, sent[t]);
        // y_t = RNN(x_t)
        Expression i_y_t = dbuilder.add_input(i_x_t);
        Expression i_r_t = affine_transform({i_bias, i_R, i_y_t});
        Expression i_err = pickneglogsoftmax(i_r_t, sent[t+1]);
        errs.push_back(i_err);
      }
    }
    return sum(errs) / L - log_prior;
  }
};

int main(int argc, char** argv) {
  dynet::initialize(argc, argv);
  if (argc != 3 && argc != 4) {
    cerr << "Usage: " << argv[0] << " corpus.txt dev.txt [model.params]\n";
    return 1;
  }
  kSOS = d.convert("<s>");
  kEOS = d.convert("</s>");
  vector<vector<int>> training, dev;
  string line;
  int tlc = 0;
  int ttoks = 0;
  cerr << "Reading training data from " << argv[1] << "...\n";
  {
    ifstream in(argv[1]);
    assert(in);
    while(getline(in, line)) {
      ++tlc;
      training.push_back(read_sentence(line, d));
      ttoks += training.back().size();
      if (training.back().front() != kSOS && training.back().back() != kEOS) {
        cerr << "Training sentence in " << argv[1] << ":" << tlc << " didn't start or end with <s>, </s>\n";
        abort();
      }
    }
    cerr << tlc << " lines, " << ttoks << " tokens, " << d.size() << " types\n";
  }
  d.freeze(); // no new word types allowed
  VOCAB_SIZE = d.size();

  int dlc = 0;
  int dtoks = 0;
  cerr << "Reading dev data from " << argv[2] << "...\n";
  {
    ifstream in(argv[2]);
    assert(in);
    while(getline(in, line)) {
      ++dlc;
      dev.push_back(read_sentence(line, d));
      dtoks += dev.back().size();
      if (dev.back().front() != kSOS && dev.back().back() != kEOS) {
        cerr << "Dev sentence in " << argv[2] << ":" << dlc << " didn't start or end with <s>, </s>\n";
        abort();
      }
    }
    cerr << dlc << " lines, " << dtoks << " tokens\n";
  }
  ostringstream os;
  os << "lm"
     << '_' << LAYERS
     << '_' << INPUT_DIM
     << '_' << HIDDEN_DIM
     << "-pid" << getpid() << ".params";
  const string fname = os.str();
  cerr << "Parameters will be written to: " << fname << endl;
  double best = 9e+99;

  ParameterCollection model;
  Trainer* sgd = nullptr;
  // bool use_momentum = false;
  // if (use_momentum)
  //   sgd = new MomentumSGDTrainer(model);
  // else
  sgd = new SimpleSGDTrainer(model);

  RNNLanguageModel<GRUBuilder> lm(model);
  //RNNLanguageModel<SimpleRNNBuilder> lm(model);
  if (argc == 4) {
    string fname = argv[3];
    TextFilePacker packer(fname);
    packer.populate(model, "model");
  }

  unsigned report_every_i = 50;
  unsigned dev_every_i_reports = 10;
  unsigned si = training.size();
  vector<unsigned> order(training.size());
  for (unsigned i = 0; i < order.size(); ++i) order[i] = i;
  bool first = true;
  int report = 0;
  unsigned lines = 0;
  while(1) {
    Timer iteration("completed in");
    double loss = 0;
    unsigned chars = 0;
    for (unsigned i = 0; i < report_every_i; ++i) {
      if (si == training.size()) {
        si = 0;
        if (first) { first = false; } else { sgd->update_epoch(); }
        cerr << "**SHUFFLE\n";
        shuffle(order.begin(), order.end(), *rndeng);
      }

      // build graph for this instance
      ComputationGraph cg;
      auto& sent = training[order[si]];
      chars += sent.size() - 1;
      ++si;
      Expression loss_expr = lm.BuildLMGraph(sent, cg);
      loss += as_scalar(cg.forward(loss_expr));
      cg.backward(loss_expr);
      sgd->update();
      ++lines;
    }
    sgd->status();
    cerr << " E = " << (loss / chars) << " ppl=" << exp(loss / chars) << ' ';

    // show score on dev data?
    report++;
    if (report % dev_every_i_reports == 0) {
      cout << endl;
      double dloss = 0;
      int dchars = 0;
      for (auto& sent : dev) {
        ComputationGraph cg;
        Expression loss_expr = lm.BuildLMGraph(sent, cg, true);
        dloss += as_scalar(cg.forward(loss_expr));
        dchars += sent.size() - 1;
      }
      if (dloss < best) {
        best = dloss;
        std::string fname_meta = fname + ".meta";
        std::remove(fname_meta.c_str()); std::remove(fname.c_str());
        TextFilePacker packer(fname);
        packer.save(model, "model");
      }
      cerr << "\n***DEV [epoch=" << (lines / (double)training.size()) << "] E = " << (dloss / dchars) << " ppl=" << exp(dloss / dchars) << ' ';
    }
  }
  delete sgd;
}

