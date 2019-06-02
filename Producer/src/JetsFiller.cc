#include "../interface/JetsFiller.h"

#include "FWCore/Framework/interface/ESHandle.h"
#include "FWCore/ServiceRegistry/interface/Service.h"
#include "FWCore/Utilities/interface/RandomNumberGenerator.h"

#include "DataFormats/PatCandidates/interface/Jet.h"
#include "DataFormats/JetReco/interface/GenJet.h"
#include "DataFormats/Math/interface/deltaR.h"

#include "CLHEP/Random/RandomEngine.h"
#include "CLHEP/Random/RandGauss.h"

#include "CondFormats/JetMETObjects/interface/JetCorrectorParameters.h"
#include "CondFormats/JetMETObjects/interface/JetCorrectionUncertainty.h"
#include "JetMETCorrections/Objects/interface/JetCorrectionsRecord.h"
#include "JetMETCorrections/Objects/interface/JetCorrector.h"
#include "JetMETCorrections/Modules/interface/JetResolution.h"

#include <cmath>
#include <stdexcept>

JetsFiller::JetsFiller(std::string const& _name, edm::ParameterSet const& _cfg, edm::ConsumesCollector& _coll) :
  FillerBase(_name, _cfg),
  jecName_(getParameter_<std::string>(_cfg, "jec", "")),
  jerName_(getParameter_<std::string>(_cfg, "jer", "")),
  csvTag_(getParameter_<std::string>(_cfg, "csv", "")),
  cmvaTag_(getParameter_<std::string>(_cfg, "cmva", "")),
  qglTag_(getParameter_<std::string>(_cfg, "qgl", "")),
  puidTag_(getParameter_<std::string>(_cfg, "puid", "")),
  deepCsvTag_(getParameter_<std::string>(_cfg, "deepCSV", "")),
  deepCmvaTag_(getParameter_<std::string>(_cfg, "deepCMVA", "")),
  outGenJets_(getParameter_<std::string>(_cfg, "pandaGenJets", "")),
  constituentsLabel_(getParameter_<std::string>(_cfg, "constituents", "")),
  R_(getParameter_<double>(_cfg, "R", 0.4)),
  minPt_(getParameter_<double>(_cfg, "minPt", 15.)),
  maxEta_(getParameter_<double>(_cfg, "maxEta", 4.7)),
  fillConstituents_(getParameter_<bool>(_cfg, "fillConstituents", false))
{
  if (_name == "chsAK4Jets")
    outputSelector_ = [](panda::Event& _event)->panda::JetCollection& { return _event.chsAK4Jets; };
  else if (_name == "puppiAK4Jets")
    outputSelector_ = [](panda::Event& _event)->panda::JetCollection& { return _event.puppiAK4Jets; };
  else if (_name == "chsAK8Jets")
    outputSelector_ = [](panda::Event& _event)->panda::JetCollection& { return _event.chsAK8Jets; };
  else if (_name == "puppiAK8Jets")
    outputSelector_ = [](panda::Event& _event)->panda::JetCollection& { return _event.puppiAK8Jets; };
  else if (_name == "chsCA15Jets")
    outputSelector_ = [](panda::Event& _event)->panda::JetCollection& { return _event.chsCA15Jets; };
  else if (_name == "puppiCA15Jets")
    outputSelector_ = [](panda::Event& _event)->panda::JetCollection& { return _event.puppiCA15Jets; };
  else
    throw edm::Exception(edm::errors::Configuration, "Unknown JetCollection output");    

  getToken_(jetsToken_, _cfg, _coll, "jets");
  if (!isRealData_) {
    getToken_(genJetsToken_, _cfg, _coll, "genJets", false);
    getToken_(rhoToken_, _cfg, _coll, "rho", "rho");
  }

  // Check the enums and map
  assert(deepProbs.size() == deepSuff::DEEP_SIZE);
}

JetsFiller::~JetsFiller()
{
  delete jecUncertainty_;
}

void
JetsFiller::branchNames(panda::utils::BranchList& _eventBranches, panda::utils::BranchList&) const
{
  _eventBranches.emplace_back(getName());

  if (isRealData_)
    _eventBranches.emplace_back("!" + getName() + ".matchedGenJet_");

  if (isRealData_ || jerName_.empty()) {
    char const* genBranches[] = {
      ".ptSmear",
      ".ptSmearUp",
      ".ptSmearDown"
    };
    for (char const* b : genBranches)
      _eventBranches.emplace_back("!" + getName() + b);
  }

  if (puidTag_.empty())
    _eventBranches.emplace_back("!" + getName() + ".puid");

  if (csvTag_.empty()) {
    _eventBranches.emplace_back("!" + getName() + ".csv");
    _eventBranches.emplace_back("!" + getName() + ".secondaryVertex_");
  }

  if (cmvaTag_.empty())
    _eventBranches.emplace_back("!" + getName() + ".cmva");

  for (auto prob : deepProbs) {
    if (deepCsvTag_.empty())
      _eventBranches.emplace_back("!" + getName() + ".deepCSV" + prob.first);
    if (deepCmvaTag_.empty())
      _eventBranches.emplace_back("!" + getName() + ".deepCMVA" + prob.first);
  }

  if (qglTag_.empty())
    _eventBranches.emplace_back("!" + getName() + ".qgl");

  if (!fillConstituents_)
    _eventBranches.emplace_back("!" + getName() + ".constituents_");
}

void
JetsFiller::fill(panda::Event& _outEvent, edm::Event const& _inEvent, edm::EventSetup const& _setup)
{
  auto& inJets(getProduct_(_inEvent, jetsToken_));

  panda::JetCollection& outJets(outputSelector_(_outEvent));

  if (!jecUncertainty_ && !jecName_.empty()) {
    edm::ESHandle<JetCorrectorParametersCollection> jecColl;
    _setup.get<JetCorrectionsRecord>().get(jecName_, jecColl);
    jecUncertainty_ = new JetCorrectionUncertainty((*jecColl)["Uncertainty"]);
  }

  GenJetView const* genJets(0);
  JME::JetResolution ptRes;
  JME::JetResolutionScaleFactor ptResSF;
  double rho(0.);
  CLHEP::RandGauss* random(0);
  
  if (!isRealData_) {
    if (!genJetsToken_.second.isUninitialized())
      genJets = &getProduct_(_inEvent, genJetsToken_);

    if (!jerName_.empty()) {
      ptRes = JME::JetResolution::get(_setup, jerName_ + "_pt");
      ptResSF = JME::JetResolutionScaleFactor::get(_setup, jerName_);

      rho = getProduct_(_inEvent, rhoToken_);
      random = new CLHEP::RandGauss(edm::Service<edm::RandomNumberGenerator>()->getEngine(_inEvent.streamID()));
    }
  }

  std::vector<edm::Ptr<reco::Jet>> ptrList;
  std::vector<edm::Ptr<reco::GenJet>> matchedGenJets;

  unsigned iJet(-1);
  for (auto& inJet : inJets) {
    ++iJet;

    if (inJet.pt() < minPt_)
      continue;

    double absEta(std::abs(inJet.eta()));
    if (absEta > 4.7)
      continue;

    auto inRef(inJets.refAt(iJet));

    // Forking for MINIAOD jets (not that we have implementation for reco::PFJet though)
    if (dynamic_cast<pat::Jet const*>(&inJet)) {
      auto& patJet(static_cast<pat::Jet const&>(inJet));

      double nhf(patJet.neutralHadronEnergyFraction());
      double nef(patJet.neutralEmEnergyFraction());
      double chf(patJet.chargedHadronEnergyFraction());
      double cef(patJet.chargedEmEnergyFraction());
      double muf(patJet.muonEnergyFraction());
      unsigned nc(patJet.chargedMultiplicity());
      unsigned nn(patJet.neutralMultiplicity());
      unsigned nd(patJet.numberOfDaughters());
      bool loose(true); // loose is not recommended any more
      bool tight(false);
      bool tightLepVeto(false);
      bool monojet(false);

      if (absEta <= 2.7) {
        tight = nhf < 0.9 && nef < 0.9 && nd > 1;
        tightLepVeto = nhf < 0.9 && nef < 0.9 && nd > 1 && muf < 0.8;
        monojet = nhf < 0.8 && nef < 0.99 && nd > 1;

        if (absEta <= 2.4) {
          tight = tight && chf > 0. && nc > 0;
          tightLepVeto = tightLepVeto && chf > 0. && nc > 0 && cef < 0.80;
          monojet = monojet && chf > 0.1 && nc > 0 && cef < 0.99;
        }
      }
      else if (absEta <= 3.)
        tightLepVeto = tight = nef < 0.99 && nef > 0.02 && nn > 2;
      else
        tightLepVeto = tight = nef < 0.9 && nhf > 0.02 && nn > 10;

      auto& outJet(outJets.create_back());

      fillP4(outJet, inJet);

      outJet.rawPt = patJet.pt() * patJet.jecFactor("Uncorrected");

      if (jecUncertainty_) {
        jecUncertainty_->setJetEta(inJet.eta());
        jecUncertainty_->setJetPt(inJet.pt());
        outJet.ptCorrUp = outJet.pt() * (1. + jecUncertainty_->getUncertainty(true));
        jecUncertainty_->setJetEta(inJet.eta());
        jecUncertainty_->setJetPt(inJet.pt());
        outJet.ptCorrDown = outJet.pt() * (1. - jecUncertainty_->getUncertainty(false));
      }

      if (!isRealData_) {
        reco::GenJet const* matchedGenJet(0);

        if (genJets) {
          unsigned iG(0);
          for (; iG != genJets->size(); ++iG) {
            auto& genJet(genJets->at(iG));
            if (reco::deltaR(genJet, inJet) < R_ * 0.5) {
              matchedGenJet = &genJet;
              break;
            }
          }
          if (iG != genJets->size())
            matchedGenJets.emplace_back(genJets->ptrAt(iG));
          else
            matchedGenJets.emplace_back();
        }

        if (!jerName_.empty()) {
          JME::JetParameters resParams({{JME::Binning::JetPt, inJet.pt()}, {JME::Binning::JetEta, inJet.eta()}, {JME::Binning::Rho, rho}});
          double res(ptRes.getResolution(resParams) * inJet.pt());

          JME::JetParameters sfParams({{JME::Binning::JetEta, inJet.eta()}});
          double sf(ptResSF.getScaleFactor(sfParams));
          double sfUp(ptResSF.getScaleFactor(sfParams, Variation::UP));
          double sfDown(ptResSF.getScaleFactor(sfParams, Variation::DOWN));

          if (matchedGenJet && std::abs(inJet.pt() - matchedGenJet->pt()) < res * 3.) {
            double dpt(inJet.pt() - matchedGenJet->pt());
            outJet.ptSmear = std::max(0., matchedGenJet->pt() + sf * dpt);
            outJet.ptSmearUp = std::max(0., matchedGenJet->pt() + sfUp * dpt);
            outJet.ptSmearDown = std::max(0., matchedGenJet->pt() + sfDown * dpt);
          }
          else {
            double resShift(std::sqrt(sf * sf - 1.));
            outJet.ptSmear = (*random)(inJet.pt(), resShift * res);
            // Smear the jet in the same direction, just with different SF
            outJet.ptSmearUp = inJet.pt() + (outJet.ptSmear - inJet.pt()) * std::sqrt(sfUp * sfUp - 1.) / resShift;
            outJet.ptSmearDown = inJet.pt() + (outJet.ptSmear - inJet.pt()) * std::sqrt(sfDown * sfDown - 1.) / resShift;
          }
        }
      }

      if (!csvTag_.empty())
        outJet.csv = patJet.bDiscriminator(csvTag_);
      if (!cmvaTag_.empty())
        outJet.cmva = patJet.bDiscriminator(cmvaTag_);

      // Fill with -0.5 if we didn't match the jets
      if (!deepCsvTag_.empty()) {
        for (auto prob : deepProbs)
          fillDeepBySwitch_(outJet, prob.second, patJet.bDiscriminator(deepCsvTag_ + ":prob" + prob.first));
      }
      if (!deepCmvaTag_.empty()) {
        for (auto prob : deepProbs)
          fillDeepBySwitch_(outJet, prob.second + deepSuff::DEEP_SIZE, patJet.bDiscriminator(deepCmvaTag_ + ":prob" + prob.first));
      }

      if (!qglTag_.empty())
        outJet.qgl = patJet.userFloat(qglTag_);

      if (!puidTag_.empty())
        outJet.puid = patJet.userFloat(puidTag_);
        
      outJet.area = inJet.jetArea();
      outJet.nhf = nhf;
      outJet.chf = chf;
      outJet.nef = nef;
      outJet.cef = cef;
      outJet.loose = loose;
      outJet.tight = tight;
      outJet.tightLepVeto = tightLepVeto;
      outJet.monojet = monojet;
    }

    ptrList.push_back(inJets.ptrAt(iJet));
  }

  // sort the output jets
  auto originalIndices(outJets.sort(panda::Particle::PtGreater));

  // export panda <-> reco mapping

  auto& objectMap(objectMap_->get<reco::Jet, panda::Jet>());
  auto& genJetMap(objectMap_->get<reco::GenJet, panda::Jet>());

  for (unsigned iP(0); iP != outJets.size(); ++iP) {
    auto& outJet(outJets[iP]);
    unsigned idx(originalIndices[iP]);
    objectMap.add(ptrList[idx], outJet);

    if (!isRealData_ && matchedGenJets.size() != 0) {
      auto& genJetPtr(matchedGenJets[idx]);
      if (genJetPtr.isNonnull())
        genJetMap.add(genJetPtr, outJet);
    }
  }

  fillDetails_(_outEvent, _inEvent, _setup);

  delete random;
}

void
JetsFiller::setRefs(ObjectMapStore const& _objectMaps)
{
  if (fillConstituents_) {
    auto& jetMap(objectMap_->get<reco::Jet, panda::Jet>());

    auto& pfMap(_objectMaps.at("pfCandidates").get<reco::Candidate, panda::PFCand>(constituentsLabel_).fwdMap);

    for (auto& link : jetMap.fwdMap) { // edm -> panda
      auto& inJet(*link.first);
      auto& outJet(*link.second);

      auto* inPATJet(dynamic_cast<pat::Jet const*>(&inJet));
      if (inPATJet == nullptr)
        continue;

      auto addPFRef([&outJet, &pfMap](reco::CandidatePtr const& _ptr) {
          reco::CandidatePtr p(_ptr);
          while (true) {
            auto&& mItr(pfMap.find(p));
            if (mItr != pfMap.end()) {
              outJet.constituents.addRef(mItr->second);
              break;
            }
            else {
              p = p->sourceCandidatePtr(0);
              if (p.isNull()) {
                // throw std::runtime_error("Constituent candidate not found in PF map");
                // With bad muon cleaning for 80, we need to allow missing constituents.
                break;
              }
            }
          }
        });
      
      for (auto&& ptr : inPATJet->daughterPtrVector())
        addPFRef(ptr);
    }
  }

  // Set the references to the secondary vertices
  if (!csvTag_.empty()) {

    auto& jetMap(objectMap_->get<reco::Jet, panda::Jet>());
    auto& svMap(_objectMaps.at("secondaryVertices").get<reco::VertexCompositePtrCandidate, panda::SecondaryVertex>());
    auto& pvMap(_objectMaps.at("vertices").get<reco::Vertex, panda::RecoVertex>());

    edm::Ptr<reco::Vertex> pv;

    float maxScore(0);
    for (auto& vtxLink : pvMap.fwdMap) {

      auto outVtx(*vtxLink.second);
      if (outVtx.score > maxScore) {

        maxScore = outVtx.score;
        pv = vtxLink.first;

      }
    }

    for (auto& jetLink : jetMap.fwdMap) {   // edm -> panda
      float maxSignificance(0);

      auto& inJet(*jetLink.first);
      auto& outJet(*jetLink.second);

      panda::SecondaryVertex* matchedSV(nullptr);

      for (auto& svLink : svMap.fwdMap) {   // edm -> panda

        auto inLocation = svLink.first->vertex();
        if (Geom::deltaR2(GlobalVector(inLocation.x() - pv->x(), inLocation.y() - pv->y(), inLocation.z() - pv->z()),
                          GlobalVector(inJet.px(), inJet.py(), inJet.pz())) < 0.09){

          auto& outSV(*svLink.second);

          if (outSV.significance > maxSignificance) {
            maxSignificance = outSV.significance;
            matchedSV = svLink.second;
          }
        }
      }

      if (matchedSV != nullptr)
        outJet.secondaryVertex.setRef(matchedSV);

    }
  }

  if (!isRealData_ && !outGenJets_.empty()) {
    auto& genJetMap(objectMap_->get<reco::GenJet, panda::Jet>().fwdMap);

    auto& genMap(_objectMaps.at(outGenJets_).get<reco::GenJet, panda::GenJet>().fwdMap);

    for (auto& link : genJetMap) {
      auto& genPtr(link.first);
      if (genMap.find(genPtr) == genMap.end())
        continue;

      auto& outJet(*link.second);
      outJet.matchedGenJet.setRef(genMap.at(genPtr));
    }
  }
}

void
JetsFiller::fillDeepBySwitch_(panda::MicroJet& outJet, const unsigned int key, float value)
{
  switch(key) {
  case deepSuff::udsg:
    outJet.deepCSVudsg = value;
    break;
  case deepSuff::b:
    outJet.deepCSVb = value;
    break;
  case deepSuff::c:
    outJet.deepCSVc = value;
    break;
  case deepSuff::bb:
    outJet.deepCSVbb = value;
    break;
  case deepSuff::cc:
    outJet.deepCSVcc = value;
    break;
  case deepSuff::udsg + deepSuff::DEEP_SIZE:
    outJet.deepCMVAudsg = value;
    break;
  case deepSuff::b + deepSuff::DEEP_SIZE:
    outJet.deepCMVAb = value;
    break;
  case deepSuff::c + deepSuff::DEEP_SIZE:
    outJet.deepCMVAc = value;
    break;
  case deepSuff::bb + deepSuff::DEEP_SIZE:
    outJet.deepCMVAbb = value;
    break;
  case deepSuff::cc + deepSuff::DEEP_SIZE:
    outJet.deepCMVAcc = value;
    break;
  default:
    std::invalid_argument("Key is too large!");
  }
}

DEFINE_TREEFILLER(JetsFiller);
