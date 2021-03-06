#include <osmtools/AreaExtractor.h>
#include <sserialize/utility/assert.h>

namespace osmtools {
namespace detail {
namespace AreaExtractor {

bool MultiPolyResolver::closedPolysFromWays(const std::vector<RawWay> & ways, std::vector<RawWay> & closedWays) {
	using std::swap;
	typedef std::unordered_set<uint32_t> ActiveWaysContainer;
	ActiveWaysContainer activeWays; //contains only ways with at least two nodes
	bool allOk = true;
	std::vector<RawWay> resultWays;
	for(uint32_t i = 0, s = (uint32_t) ways.size(); i < s; ++i) {
		if (ways[i].size() > 1)
			activeWays.insert(i);
	}
		
	while (activeWays.size()) {
		RawWay activeWay = ways[*activeWays.begin()];
		activeWays.erase(activeWays.begin());
		for(ActiveWaysContainer::iterator it(activeWays.begin()), end(activeWays.end()); activeWay.front() != activeWay.back() && it != end;) {
			bool matched = true;
			const RawWay & otherWay = ways[*it];
			//now check if we can append this way to our current active way
			if (activeWay.back() == otherWay.front()) {
				activeWay.insert(activeWay.end(), otherWay.begin()+1, otherWay.end());
			}
			else if (activeWay.back() == otherWay.back()) {
				activeWay.insert(activeWay.end(), otherWay.rbegin()+1, otherWay.rend());
			}
			else if (activeWay.front() == otherWay.front()) {
				RawWay tmp;
				tmp.reserve(activeWay.size()+otherWay.size());
				tmp.insert(tmp.end(), otherWay.rbegin(), otherWay.rend());
				tmp.insert(tmp.end(), activeWay.begin()+1, activeWay.end());
				swap(activeWay, tmp);
			}
			else if (activeWay.front() == otherWay.back()) {
				RawWay tmp;
				tmp.reserve(activeWay.size()+otherWay.size());
				tmp.insert(tmp.end(), otherWay.begin(), otherWay.end());
				tmp.insert(tmp.end(), activeWay.begin()+1, activeWay.end());
				swap(activeWay, tmp);
			}
			else {
				matched = false;
			}
			
			if (matched) {
				activeWays.erase(it);
				it = activeWays.begin();
			}
			else {
				++it;
			}
		}
		//check if way is an area and closed
		if (activeWay.size() >= 4 && activeWay.front() == activeWay.back()) {
			resultWays.push_back(activeWay);
		}
		else {
// 			std::cout << "Unclosed way detected, discarding\n";
			allOk = false;
		}
	}
	
	closedWays.swap(resultWays);
	
	return allOk;
}
	
bool MultiPolyResolver::multiPolyFromWays(const std::vector<RawWay> & innerIn, const std::vector<RawWay> & outerIn, std::vector<RawWay> & innerOut, std::vector<RawWay> & outerOut) {
	using std::swap;
	if (!outerIn.size()) {
		return false;
	}
	
	std::vector<RawWay> outer;
	bool ok = closedPolysFromWays(outerIn, outer);
	swap(outer, outerOut);
	
	if (innerIn.size()) {
		std::vector<RawWay> inner;
		ok = closedPolysFromWays(innerIn, inner) && ok;
		swap(inner, innerOut);
	}
	
	return ok;
}

}}//end namespace detail::AreaExtractor


AreaExtractor::ExtractionFunctorBase::ExtractionFunctorBase(AreaExtractor::Context* ctx) :
ctx(ctx),
pbi(0)
{
	generics::RCPtr<osmpbf::AbstractTagFilter> tmp( detail::AreaExtractor::Base::createExtractionFilter(ctx->extractionTypes) );
	if (ctx->externalFilter.get()) {
		mainFilter.reset( osmpbf::newAnd(ctx->externalFilter->copy(), tmp.get()) );
	}
	else {
		mainFilter = tmp;
	}
}

AreaExtractor::ExtractionFunctorBase::ExtractionFunctorBase(const AreaExtractor::ExtractionFunctorBase & other) :
ctx(other.ctx),
pbi(0)
{
	generics::RCPtr<osmpbf::AbstractTagFilter> tmp( detail::AreaExtractor::Base::createExtractionFilter(ctx->extractionTypes) );
	if (ctx->externalFilter.get()) {
		mainFilter.reset( osmpbf::newAnd(ctx->externalFilter->copy(), tmp.get()) );
	}
	else {
		mainFilter = tmp;
	}
}

void AreaExtractor::ExtractionFunctorBase::assignInputAdaptor(osmpbf::PrimitiveBlockInputAdaptor& pbi) {
	if (this->pbi != &pbi) {
		mainFilter->assignInputAdaptor(&pbi);
		this->pbi = &pbi;
	}
}

void AreaExtractor::NodeGatherer::operator()(osmpbf::PrimitiveBlockInputAdaptor& pbi) {
	ctx->pinfo(ctx->inFile.dataPosition());
	if (!pbi.nodesSize()) {
		return;
	}
	
	for(osmpbf::INodeStream node(pbi.getNodeStream()); !node.isNull(); node.next()) {
		int64_t nid = node.id();
		if (ctx->nodes.count( nid ) ) {
			Point & gp = ctx->nodes.at(nid);
			gp.first = node.latd();
			gp.second = node.lond();
			if (ctx->snapGeometry) {
				gp.first = sserialize::spatial::GeoPoint::snapLat(gp.first);
				gp.second = sserialize::spatial::GeoPoint::snapLon(gp.second);
			}
		}
	}
}

void AreaExtractor::WayRefsExtractor::operator()(osmpbf::PrimitiveBlockInputAdaptor& pbi) {
	ctx->pinfo(ctx->inFile.dataPosition());
	if (!pbi.waysSize())
		return;
	
	assignInputAdaptor(pbi);

	if (!mainFilter->rebuildCache()) {
		return;
	}
	
	uint32_t myRelevantWays = 0;
	myRefs.clear();
	
	for(osmpbf::IWayStream way(pbi.getWayStream()); !way.isNull(); way.next()) {
		if (way.refsSize() > 4 && *way.refBegin() == *(way.refBegin()+(way.refsSize()-1)) && mainFilter->matches(way)) {
			myRefs.insert(way.refBegin(), way.refEnd());
			++myRelevantWays;
		}
	}
	ctx->relevantWaysSize += myRelevantWays;
	std::lock_guard<std::mutex> lck(ctx->nodesLock);
	ctx->nodes.reserve(ctx->nodes.size()+myRefs.size());
	for(int64_t x : myRefs) {
		ctx->nodes[x];
	}
	myRefs.clear();
}

void AreaExtractor::WayExtractor::operator()(osmpbf::PrimitiveBlockInputAdaptor& pbi) {
	ctx->pinfo(ctx->inFile.dataPosition());
	if (!pbi.waysSize()) {
		return;
	}
	
	assignInputAdaptor(pbi);

	if (!mainFilter->rebuildCache()) {
		return;
	}
	
	uint32_t myAssembledRelevantWays = 0;
	
	for(osmpbf::IWayStream way(pbi.getWayStream()); !way.isNull(); way.next()) {
		if (way.refsSize() > 4 && *way.refBegin() == *(way.refBegin()+(way.refsSize()-1)) && mainFilter->matches(way)) {
			std::shared_ptr<sserialize::spatial::GeoPolygon> gpo( new sserialize::spatial::GeoPolygon() );
			sserialize::spatial::GeoPolygon::PointsContainer & gpops = gpo->points();
			gpops.reserve(way.refsSize());
			for(osmpbf::IWayStream::RefIterator it(way.refBegin()), end(way.refEnd()); it != end; ++it) {
				int64_t wr = *it;
				if (ctx->nodes.count(wr)) {
					gpops.push_back(ctx->nodes[wr]);
				}
				else {
					std::cout << "Way has missing node\n";
					break;
				}
			}
			if (gpo->points().size()) {
				gpo->recalculateBoundary();
				cb->operator()(gpo, way);
				++myAssembledRelevantWays;
			}
		}
	}
	ctx->assembledRelevantWays += myAssembledRelevantWays;
}

void AreaExtractor::RelationWaysExtractor::operator()(osmpbf::PrimitiveBlockInputAdaptor& pbi) {
	ctx->pinfo(ctx->inFile.dataPosition());
	if (!pbi.relationsSize()) {
		return;
	}
	
	assignInputAdaptor(pbi);

	if (!mainFilter->rebuildCache()) {
		return;
	}
	
	uint32_t myRelevantRelations = 0;
	myWayIds.clear();
	
	for (osmpbf::IRelationStream rel = pbi.getRelationStream(); !rel.isNull(); rel.next()) {
		if (mainFilter->matches(rel)) {
			for(osmpbf::IMemberStream mem = rel.getMemberStream(); !mem.isNull(); mem.next()) {
				if (mem.type() == osmpbf::WayPrimitive) {
					myWayIds.insert(mem.id());
				}
			}
			++myRelevantRelations;
		}
	}
	ctx->relevantRelationsSize += myRelevantRelations;
	std::lock_guard<std::mutex> lck(ctx->rawWaysLock);
	ctx->rawWays.reserve(ctx->rawWays.size()+myWayIds.size());
	for(int64_t x : myWayIds) {
		ctx->rawWays[x];
	}
	myWayIds.clear();
}

void AreaExtractor::RelationWayNodeRefsExtractor::operator()(osmpbf::PrimitiveBlockInputAdaptor& pbi) {
	ctx->pinfo(ctx->inFile.dataPosition());
	if (!pbi.waysSize()) {
		return;
	}
	
	myWayRefs.clear();
	
	for (osmpbf::IWayStream way = pbi.getWayStream(); !way.isNull(); way.next()) {
		int64_t wid = way.id();
		if (ctx->rawWays.count(wid)) {
			detail::AreaExtractor::MultiPolyResolver::RawWay & rw = ctx->rawWays[wid];
			rw.insert(rw.end(), way.refBegin(), way.refEnd());
			rw.shrink_to_fit();
			myWayRefs.insert(way.refBegin(), way.refEnd());
		}
	}
	std::lock_guard<std::mutex> lck(ctx->nodesLock);
	ctx->nodes.reserve(ctx->nodes.size() + myWayRefs.size());
	for(int64_t x : myWayRefs) {
		ctx->nodes[x];
	}
}

void AreaExtractor::RelationExtractor::operator()(osmpbf::PrimitiveBlockInputAdaptor& pbi) {
	ctx->pinfo(ctx->inFile.dataPosition());
	if (!pbi.relationsSize()) {
		return;
	}
	
	assignInputAdaptor(pbi);

	if (!mainFilter->rebuildCache()) {
		return;
	}
	
	uint32_t myAssembledRelevantRelations = 0;
	
	for(osmpbf::IRelationStream rel(pbi.getRelationStream()); !rel.isNull(); rel.next()) {
		if (!mainFilter->matches(rel)) {
			continue;
		}
		std::vector< detail::AreaExtractor::MultiPolyResolver::RawWay > outerWays, innerWays;
		
		bool allWaysAvailable = true;
		
		for(osmpbf::IMemberStream mem = rel.getMemberStream(); !mem.isNull(); mem.next()) {
			if (mem.type() == osmpbf::WayPrimitive) {
				if (ctx->rawWays.count(mem.id())) {
					const detail::AreaExtractor::MultiPolyResolver::RawWay & rw = ctx->rawWays[ mem.id() ];
					
					if (!rw.size()) {
						continue;
					}
					
					if (mem.role() == "outer" || mem.role() == "" || mem.role() == "exclave" || mem.role() == "Outer" || mem.role() == "outer:FIXME") {
						outerWays.push_back(rw);
					}
					else if (mem.role() == "inner" || mem.role() == "enclave") {
						innerWays.push_back(rw);
					}
					else if (ctx->verbose) {
						std::cout << "Illegal role in relation " << rel.id() << ": " << mem.role() << '\n';
					}
				}
				else {
					allWaysAvailable = false;
				}
			}
		}
		if (outerWays.size() && !detail::AreaExtractor::MultiPolyResolver::multiPolyFromWays(innerWays, outerWays, innerWays, outerWays) && allWaysAvailable) {
			if (ctx->verbose) {
				std::cerr << "Failed to fully create MultiPolygon from multiple ways for relation " << rel.id() << '\n';
			}
		}
		if (outerWays.size()) {
			std::shared_ptr<sserialize::spatial::GeoRegion> gmpo( detail::AreaExtractor::MultiPolyResolver::multiPolyFromClosedWays(innerWays, outerWays, ctx->nodes) );
			cb->operator()(gmpo, rel);
			++myAssembledRelevantRelations;
		}
	}
	
	ctx->assembledRelevantRelations += myAssembledRelevantRelations;
}

}//end namespace
