/*
  Copyright 2010 SINTEF ICT, Applied Mathematics.
  Copyright 2010 Statoil ASA.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <config.h>

#include <opm/common/utility/platform_dependent/disable_warnings.h>

#include <dune/common/version.hh>

#include <dune/common/parallel/mpihelper.hh>

#include <opm/common/utility/platform_dependent/reenable_warnings.h>

#include <opm/common/utility/numeric/MonotCubicInterpolator.hpp>
#include <opm/input/eclipse/Units/Units.hpp>

#include <opm/output/eclipse/EclipseGridInspector.hpp>

#include <opm/porsol/common/setupBoundaryConditions.hpp>

#include <opm/upscaling/SinglePhaseUpscaler.hpp>
#include <opm/upscaling/CornerpointChopper.hpp>

#include <cfloat>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <sstream>

#include <sys/utsname.h>

#include <random>

int main(int argc, char** argv)
try
{
    if (argc == 1) {
        std::cout << "Usage: cpchop gridfilename=filename.grdecl [subsamples=10] [ilen=5] [jlen=5] " << std::endl;
        std::cout << "       [zlen=5] [imin=] [imax=] [jmin=] [jmax=] [upscale=true] [bc=fixed]" << std::endl;
        std::cout << "       [resettoorigin=true] [seed=111] [minperm=1e-9] " << std::endl;
        std::cout << "       [dips=false] [azimuthdisplacement=] [satnumvolumes=false] [mincellvolume=1e-9]" << std::endl;
        std::cout << "       [filebase=] [resultfile=] [endpoints=false] [cappres=false]" << std::endl;
        std::cout << "       [rock_list=] [anisotropicrocks=false]" << std::endl;
        exit(1);
    }

    Dune::MPIHelper::instance(argc, argv);

    Opm::ParameterGroup param(argc, argv);
    std::string gridfilename = param.get<std::string>("gridfilename");
    Opm::CornerPointChopper ch(gridfilename);

    // The cells with i coordinate in [imin, imax) are included, similar for j.
    // The z limits may be changed inside the chopper to match actual min/max z.
    const int* dims = ch.dimensions();
    int imin = param.getDefault("imin", 0);
    int imax = param.getDefault("imax", dims[0]);
    int jmin = param.getDefault("jmin", 0);
    int jmax = param.getDefault("jmax", dims[1]);
    double zmin = param.getDefault("zmin", ch.zLimits().first);
    double zmax = param.getDefault("zmax", ch.zLimits().second);
    int subsamples = param.getDefault("subsamples", 1);
    int ilen = param.getDefault("ilen", imax - imin);
    int jlen = param.getDefault("jlen", jmax - jmin);
    double zlen = param.getDefault("zlen", zmax - zmin);
    bool upscale = param.getDefault("upscale", true);
    std::string bc = param.getDefault<std::string>("bc", "fixed");
    bool resettoorigin = param.getDefault("resettoorigin", true);
    std::mt19937::result_type userseed = param.getDefault("seed", 0);
    bool use_random = param.getDefault("use_random", true);

    int outputprecision = param.getDefault("outputprecision", 8);
    std::string filebase = param.getDefault<std::string>("filebase", "");
    std::string resultfile = param.getDefault<std::string>("resultfile", "");

    double minperm = param.getDefault("minperm", 1e-9);
    double minpermSI = Opm::unit::convert::from(minperm, Opm::prefix::milli*Opm::unit::darcy);

    // Following two options are for dip upscaling (slope of cell top and bottom edges)
    bool dips = param.getDefault("dips", false);  // whether to do dip averaging
    double azimuthdisplacement = param.getDefault("azimuthdisplacement", 0.0);  // posibility to add/subtract a value to/from azimuth for dip plane.
    double mincellvolume = param.getDefault("mincellvolume", 1e-9); // ignore smaller cells for dip calculations

    bool satnumvolumes = param.getDefault("satnumvolumes", false); // whether to count volumes pr. satnum

    // upscaling of endpoints and capillary pressure
    // Conversion factor, multiply mD numbers with this to get m² numbers
    const double milliDarcyToSqMetre =
        Opm::unit::convert::to(1.0*Opm::prefix::milli*Opm::unit::darcy,
                               Opm::unit::square(Opm::unit::meter));

    // Input for surfaceTension is dynes/cm, SI units are Joules/square metre
    const double surfaceTension = param.getDefault("surfaceTension", 11.0) * 1e-3; // multiply with 10^-3 to obtain SI units 

    bool endpoints = param.getDefault("endpoints", false); // whether to upscale saturation endpoints
    bool cappres = param.getDefault("cappres", false); // whether to upscale capillary pressure
    if (cappres) { endpoints = true; }
    std::string rock_list = param.getDefault<std::string>("rock_list", "no_list");
    bool anisorocks = param.getDefault("anisotropicrocks", false);
    std::vector<std::vector<double> > rocksatendpoints_;
    std::vector<std::vector<double> > jfuncendpoints_; // Used if isotropic rock input
    int nsatpoints = 5; // nuber of saturation points in upscaled capillary pressure function per subsample
    double saturationThreshold = 0.00001;

    // For isotropic input rocks:
    std::vector<Opm::MonotCubicInterpolator> InvJfunctions; // Holds the inverse of the loaded J-functions.
    // For anisotropic input rocks:
    std::vector<Opm::MonotCubicInterpolator> SwPcfunctions; // Holds Sw(Pc) for each rocktype.

    // Read rock data from files specifyed in rock_list
    if (endpoints) {
        if (!rock_list.compare("no_list")) {
            std::cout << "Can't do endponts without rock list (" << rock_list << ")" << std::endl;
            throw std::exception();
        }
        // Code copied from ReservoirPropertyCommon.hpp for file reading
        std::ifstream rl(rock_list.c_str());
        if (!rl) {
            OPM_THROW(std::runtime_error, "Could not open file " << rock_list);
        }
        int num_rocks = -1;
        rl >> num_rocks;
        assert(num_rocks >= 1);
        rocksatendpoints_.resize(num_rocks, std::vector<double>(2, 0.0));
        jfuncendpoints_.resize(num_rocks, std::vector<double>(2, 0.0));
        // Loop through rock files defined in rock_list and store the data we need
        for (int i = 0; i < num_rocks; ++i) {
            std::string spec;
            while (spec.empty()) {
                std::getline(rl, spec);
            }
            // Read the contents of the i'th rock
            std::istringstream specstream(spec);
	    std::string rockname;
	    specstream >> rockname;
            std::string rockfilename = rockname;

            std::ifstream rock_stream(rockfilename.c_str());
            if (!rock_stream) {
                OPM_THROW(std::runtime_error, "Could not open file " << rockfilename);
            }
            
            if (! anisorocks) { //Isotropic input rocks (Sw Krw Kro J)
                Opm::MonotCubicInterpolator Jtmp;
                try {
                    Jtmp = Opm::MonotCubicInterpolator(rockname, 1, 4); 
                }
                catch (const char * errormessage) {
                    std::cerr << "Error: " << errormessage << std::endl;
                    std::cerr << "Check filename" << std::endl;
                    exit(1);
                }
                
                // Invert J-function, now we get saturation as a function of pressure:
                if (Jtmp.isStrictlyMonotone()) {
                    InvJfunctions.push_back(Opm::MonotCubicInterpolator(Jtmp.get_fVector(), Jtmp.get_xVector()));
                }
                else {
                    std::cerr << "Error: Jfunction " << i+1 << " in rock file " << rockname << " was not invertible." << std::endl;
                    exit(1);
                }
                
                jfuncendpoints_[i][0] = Jtmp.getMinimumX().second;
                jfuncendpoints_[i][1] = Jtmp.getMaximumX().second;
                rocksatendpoints_[i][0] = Jtmp.getMinimumX().first;
                rocksatendpoints_[i][1] = Jtmp.getMaximumX().first;
                if (rocksatendpoints_[i][0] < 0 || rocksatendpoints_[i][0] > 1) {
                    OPM_THROW(std::runtime_error, "Minimum rock saturation (" << rocksatendpoints_[i][0] << ") not sane for rock " 
                          << rockfilename << "." << std::endl << "Did you forget to specify anisotropicrocks=true ?");  
                }
            }
            else { //Anisotropic input rocks (Pc Sw Krxx Kryy Krzz)
                Opm::MonotCubicInterpolator Pctmp;
                try {
                    Pctmp = Opm::MonotCubicInterpolator(rockname, 2, 1);
                }
                catch (const char * errormessage) {
                    std::cerr << "Error: " << errormessage << std::endl;
                    std::cerr << "Check filename and columns 1 and 2 (Pc and Sw)" << std::endl;
                    exit(1);
                }
                if (cappres) {
                    // Invert Pc(Sw) curve into Sw(Pc):
                    if (Pctmp.isStrictlyMonotone()) {
                        SwPcfunctions.push_back(Opm::MonotCubicInterpolator(Pctmp.get_fVector(), Pctmp.get_xVector()));
                    }
                    else {
                        std::cerr << "Error: Pc(Sw) curve " << i+1 << " in rock file " << rockname << " was not invertible." << std::endl;
                        exit(1);
                    }
                }
                rocksatendpoints_[i][0] = Pctmp.getMinimumX().first;
                rocksatendpoints_[i][1] = Pctmp.getMaximumX().first;
            }          
        }
    }

    if (param.has("z_tolerance")) {
        std::cerr << "****** Warning: z_tolerance parameter is obsolete, use PINCH in deck input instead\n";
    }
    double residual_tolerance = param.getDefault("residual_tolerance", 1e-8);
    int linsolver_verbosity = param.getDefault("linsolver_verbosity", 0);
    int linsolver_type = param.getDefault("linsolver_type", 3);

    //  Guarantee initialization
    double Pcmax = -DBL_MAX, Pcmin = DBL_MAX;

    // Check that we do not have any user input
    // that goes outside the coordinates described in
    // the cornerpoint file (runtime-exception will be thrown in case of error)
    ch.verifyInscribedShoebox(imin, ilen, imax,
			      jmin, jlen, jmax,
			      zmin, zlen, zmax);

    std::mt19937 gen;

    // Seed the random number generators with the current time, unless specified on command line
    // Warning: Current code does not allow 0 for the seed!!
    std::mt19937::result_type autoseed = time(NULL);
    if (userseed == 0) {
        gen.seed(autoseed);
    }
    else {
        gen.seed(userseed);
    }

    Opm::SinglePhaseUpscaler::BoundaryConditionType bctype = Opm::SinglePhaseUpscaler::Fixed;
    bool isFixed, isPeriodic;
    isFixed = isPeriodic = false;
    if (upscale) {
        if (bc == "fixed") {
            isFixed = true;
            bctype = Opm::SinglePhaseUpscaler::Fixed;
        }
        else if (bc == "periodic") {
            isPeriodic = true;
            bctype = Opm::SinglePhaseUpscaler::Periodic;
        }
        else {
            std::cout << "Boundary condition type (bc=" << bc << ") not allowed." << std::endl;
            std::cout << "Only bc=fixed or bc=periodic implemented." << std::endl;
            throw std::exception();
        }
    }

    // Check for unused parameters (potential typos).
    if (param.anyUnused()) {
	std::cout << "*****     WARNING: Unused parameters:     *****\n";
	param.displayUsage();
    }
    
    // Note that end is included in interval for uniform_int.
    std::uniform_int_distribution<> disti(imin, imax - ilen);
    std::uniform_int_distribution<> distj(jmin, jmax - jlen);
    std::uniform_real_distribution<> distz(zmin, std::max(zmax - zlen, zmin+1e-6));
    auto ri = [&disti, &gen] { return disti(gen); };
    auto rj = [&distj, &gen] { return distj(gen); };
    auto rz = [&distz, &gen] { return distz(gen); };

    // Storage for results
    std::vector<double> porosities;
    std::vector<double> netporosities;
    std::vector<double> ntgs;
    std::vector<double> swcrs;
    std::vector<double> sowcrs;
    std::vector<double> permxs;
    std::vector<double> permys;
    std::vector<double> permzs;
    std::vector<double> permyzs;
    std::vector<double> permxzs;
    std::vector<double> permxys;
    std::vector<double> minsws, maxsws;
    std::vector<std::vector<double> > pcvalues;
    std::vector<double> dipangs, azimuths;

    // Initialize a matrix for subsample satnum volumes. 
    // Outer index is subsample index, inner index is SATNUM-value
    std::vector<std::vector<double> > rockvolumes; 
    int maxSatnum = 0; // This value is determined from the chopped cells.

    int finished_subsamples = 0; // keep explicit count of successful subsamples
    for (int sample = 1; sample <= subsamples; ++sample) {
        int istart = use_random ? ri() : 0;
        int jstart = use_random ? rj() : 0;
        double zstart = use_random ? rz() : zmin;
        ch.chop(istart, istart + ilen, jstart, jstart + jlen, zstart, zstart + zlen, resettoorigin);
        std::string subsampledgrdecl = filebase;

        // Output grdecl-data to file if a filebase is supplied.
        if (filebase != "") {
            std::ostringstream oss;
            if ((size_t) subsamples > 1) { // Only add number to filename if more than one sample is asked for
                oss << 'R' << std::setw(4) << std::setfill('0') << sample;
                subsampledgrdecl += oss.str();
            }
            subsampledgrdecl += ".grdecl";
            ch.writeGrdecl(subsampledgrdecl);
        }

        try { /* The upscaling may fail to converge on icky grids, lets just pass by those */
            if (upscale) {
                auto subdeck = ch.subDeck();
                Opm::SinglePhaseUpscaler upscaler;
                
                upscaler.init(subdeck, bctype, minpermSI,
                              residual_tolerance, linsolver_verbosity, linsolver_type, false);

                Opm::SinglePhaseUpscaler::permtensor_t upscaled_K = upscaler.upscaleSinglePhase();
                upscaled_K *= (1.0/(Opm::prefix::milli*Opm::unit::darcy));


                porosities.push_back(upscaler.upscalePorosity());
                if (ch.hasNTG()) {
                    netporosities.push_back(upscaler.upscaleNetPorosity());
                    ntgs.push_back(upscaler.upscaleNTG());                    
                }
                if (ch.hasSWCR()) {
                    swcrs.push_back(upscaler.upscaleSWCR(ch.hasNTG()));
                }
                if (ch.hasSOWCR()) {
                    sowcrs.push_back(upscaler.upscaleSOWCR(ch.hasNTG()));
                }
                permxs.push_back(upscaled_K(0,0));
                permys.push_back(upscaled_K(1,1));
                permzs.push_back(upscaled_K(2,2));
                permyzs.push_back(upscaled_K(1,2));
                permxzs.push_back(upscaled_K(0,2));
                permxys.push_back(upscaled_K(0,1));
            }

            if (endpoints) {
                // Calculate minimum and maximum water volume in each cell based on input pc-curves per rock type
                // Create single-phase upscaling object to get poro and perm values from the grid
                auto subdeck = ch.subDeck();
                std::vector<double>  perms = subdeck["PERMX"].back().getRawDoubleData();
                Opm::SinglePhaseUpscaler upscaler;                
                upscaler.init(subdeck, bctype, minpermSI,
                              residual_tolerance, linsolver_verbosity, linsolver_type, false);
                std::vector<int>   satnums = subdeck["SATNUM"].back().getIntData();
                std::vector<double>  poros = subdeck["PORO"].back().getSIDoubleData();
                std::vector<double> cellVolumes, cellPoreVolumes;
                cellVolumes.resize(satnums.size(), 0.0);
                cellPoreVolumes.resize(satnums.size(), 0.0);
                int tesselatedCells = 0;
                //double maxSinglePhasePerm = 0;
                double Swirvolume = 0;
                double Sworvolume = 0;
                const std::vector<int>& ecl_idx = upscaler.grid().globalCell();
                Dune::CpGrid::Codim<0>::LeafIterator c = upscaler.grid().leafbegin<0>();
                for (; c != upscaler.grid().leafend<0>(); ++c) {
                    unsigned int cell_idx = ecl_idx[c->index()];
                    if (satnums[cell_idx] > 0) { // Satnum zero is "no rock"
                        cellVolumes[cell_idx] = c->geometry().volume();
                        cellPoreVolumes[cell_idx] = cellVolumes[cell_idx] * poros[cell_idx];
                        double Pcmincandidate = 0.0, Pcmaxcandidate = 0.0, minSw, maxSw;
                        if (!anisorocks) {
                            if (cappres) {
                                Pcmincandidate = jfuncendpoints_[int(satnums[cell_idx])-1][1]
                                    / sqrt(perms[cell_idx] * milliDarcyToSqMetre/poros[cell_idx]) * surfaceTension;
                                Pcmaxcandidate = jfuncendpoints_[int(satnums[cell_idx])-1][0]
                                    / sqrt(perms[cell_idx] * milliDarcyToSqMetre/poros[cell_idx]) * surfaceTension;
                            }
                            minSw = rocksatendpoints_[int(satnums[cell_idx])-1][0];
                            maxSw = rocksatendpoints_[int(satnums[cell_idx])-1][1];
                        }
                        else { // anisotropic input, we do not to J-function scaling
                            if (cappres) {
                                Pcmincandidate = SwPcfunctions[int(satnums[cell_idx])-1].getMinimumX().first;
                                Pcmaxcandidate = SwPcfunctions[int(satnums[cell_idx])-1].getMaximumX().first;
                            }
                            minSw = rocksatendpoints_[int(satnums[cell_idx])-1][0];
                            maxSw = rocksatendpoints_[int(satnums[cell_idx])-1][1];
                        }
                        if (cappres) {
                            Pcmin = std::min(Pcmincandidate, Pcmin);
                            Pcmax = std::max(Pcmaxcandidate, Pcmax);
                        }
                        Swirvolume += minSw * cellPoreVolumes[cell_idx];
                        Sworvolume += maxSw * cellPoreVolumes[cell_idx];
                    }
                    ++tesselatedCells; // keep count.
                }

                // If upscling=false, we still (may) want to have porosities together with endpoints
                if (!upscale) {
                    porosities.push_back(upscaler.upscalePorosity());
                }

                // Total porevolume and total volume -> upscaled porosity:
                double poreVolume = std::accumulate(cellPoreVolumes.begin(), 
                                                    cellPoreVolumes.end(),
                                                    0.0);
                double Swir = Swirvolume/poreVolume;
                double Swor = Sworvolume/poreVolume;
                minsws.push_back(Swir);
                maxsws.push_back(Swor);
                if (cappres) {
                    // Upscale capillary pressure function
                    Opm::MonotCubicInterpolator WaterSaturationVsCapPressure;
                    double largestSaturationInterval = Swor-Swir;
                    double Ptestvalue;
                    while (largestSaturationInterval > (Swor-Swir)/double(nsatpoints)) {
                        if (Pcmax == Pcmin) {
                            // This is a dummy situation, we go through once and then 
                            // we are finished (this will be triggered by zero permeability)
                            Ptestvalue = Pcmin;
                            largestSaturationInterval = 0;
                        }
                        else if (WaterSaturationVsCapPressure.getSize() == 0) {
                            /* No data values previously computed */
                            Ptestvalue = Pcmax;
                        }
                        else if (WaterSaturationVsCapPressure.getSize() == 1) {
                            /* If only one point has been computed, it was for Pcmax. So now
                               do Pcmin */
                            Ptestvalue = Pcmin;
                        }
                        else {
                            /* Search for largest saturation interval in which there are no
                               computed saturation points (and estimate the capillary pressure
                               that will fall in the center of this saturation interval)
                            */
                            std::pair<double,double> SatDiff = WaterSaturationVsCapPressure.getMissingX();
                            Ptestvalue = SatDiff.first;
                            largestSaturationInterval = SatDiff.second;
                        }
                        // Check for saneness of Ptestvalue:
                        if (std::isnan(Ptestvalue) || std::isinf(Ptestvalue)) {
                            std::cerr << "ERROR: Ptestvalue was inf or nan" << std::endl;
                            break; // Jump out of while-loop, just print out the results
                            // up to now and exit the program
                        }

                        double waterVolume = 0.0;
                        for (unsigned int i = 0; i < ecl_idx.size(); ++i) {
                            unsigned int cell_idx = ecl_idx[i];
                            double waterSaturationCell = 0.0;
                            if (satnums[cell_idx] > 0) { // handle "no rock" cells with satnum zero
                                double PtestvalueCell;
                                
                                PtestvalueCell = Ptestvalue;
                                
                                if (!anisorocks) {   
                                    double Jvalue = sqrt(perms[cell_idx] * milliDarcyToSqMetre /poros[cell_idx]) * PtestvalueCell / surfaceTension;
                                    waterSaturationCell 
                                        = InvJfunctions[int(satnums[cell_idx])-1].evaluate(Jvalue);
                                }
                                else { // anisotropic_input, then we do not do J-function-scaling
                                    waterSaturationCell = SwPcfunctions[int(satnums[cell_idx])-1].evaluate(PtestvalueCell);
                                }
                            }
                            waterVolume += waterSaturationCell  * cellPoreVolumes[cell_idx];
                        }
                        WaterSaturationVsCapPressure.addPair(Ptestvalue, waterVolume/poreVolume);
                    }
                    WaterSaturationVsCapPressure.chopFlatEndpoints(saturationThreshold);
                    std::vector<double> wattest = WaterSaturationVsCapPressure.get_fVector();
                    std::vector<double> cprtest = WaterSaturationVsCapPressure.get_xVector();
                    Opm::MonotCubicInterpolator CapPressureVsWaterSaturation(WaterSaturationVsCapPressure.get_fVector(), 
                                                                        WaterSaturationVsCapPressure.get_xVector());
                    std::vector<double> pcs;
                    for (int satp=0; satp<nsatpoints; ++satp) {
                        pcs.push_back(CapPressureVsWaterSaturation.evaluate(Swir+(Swor-Swir)/(nsatpoints-1)*satp));
                    }
                    pcvalues.push_back(pcs);
                }
                
            }


	    if (dips) {
		auto subdeck = ch.subDeck();
		std::vector<int>  griddims(3);
		const auto& specgridRecord = subdeck["SPECGRID"].back().getRecord(0);
		griddims[0] = specgridRecord.getItem("NX").get< int >(0);
		griddims[1] = specgridRecord.getItem("NY").get< int >(0);
		griddims[2] = specgridRecord.getItem("NZ").get< int >(0);

		std::vector<double> xdips_subsample, ydips_subsample;

		Opm::EclipseGridInspector gridinspector(subdeck);
		for (int k=0; k < griddims[2]; ++k) {
		    for (int j=0; j < griddims[1]; ++j) {
			for (int i=0; i < griddims[0]; ++i) {
			    if (gridinspector.cellVolumeVerticalPillars(i, j, k) > mincellvolume) {
				std::pair<double,double> xydip = gridinspector.cellDips(i, j, k);                       
				xdips_subsample.push_back(xydip.first);
				ydips_subsample.push_back(xydip.second);
			    }
			}
		    }
		}


                //  double azimuth = atan(xydip.first/xydip.second);
                //              double dip = acos(1.0/sqrt(pow(xydip.first,2.0)+pow(xydip.second,2.0)+1.0));
                //	dips_subsample.push_back( xydip.first );
                //	azims_subsample.push_back(atan(xydip.first/xydip.second));         

		// Average xdips and ydips
		double xdipaverage = accumulate(xdips_subsample.begin(), xdips_subsample.end(), 0.0)/xdips_subsample.size();
		double ydipaverage = accumulate(ydips_subsample.begin(), ydips_subsample.end(), 0.0)/ydips_subsample.size();

                // Convert to dip and azimuth
                double azimuth = atan(xdipaverage/ydipaverage)+azimuthdisplacement;
                double dip = acos(1.0/sqrt(pow(xdipaverage,2.0)+pow(ydipaverage,2.0)+1.0));
                dipangs.push_back(dip);
                azimuths.push_back(azimuth);                    
	    }

	    if (satnumvolumes) {
		auto subdeck = ch.subDeck();
		Opm::EclipseGridInspector subinspector(subdeck);
		std::vector<int>  griddims(3);
		const auto& specgridRecord = subdeck["SPECGRID"].back().getRecord(0);
		griddims[0] = specgridRecord.getItem("NX").get< int >(0);
		griddims[1] = specgridRecord.getItem("NY").get< int >(0);
		griddims[2] = specgridRecord.getItem("NZ").get< int >(0);

		int number_of_subsamplecells = griddims[0] * griddims[1] * griddims[2];

		// If SATNUM is non-existent in input grid, this will fail:
		std::vector<int> satnums = subdeck["SATNUM"].back().getIntData();

		std::vector<double> rockvolumessubsample;
		for (int cell_idx=0; cell_idx < number_of_subsamplecells; ++cell_idx) {
		    maxSatnum = std::max(maxSatnum, int(satnums[cell_idx]));
		    rockvolumessubsample.resize(maxSatnum); // Ensure long enough vector
		    rockvolumessubsample[int(satnums[cell_idx])-1] += subinspector.cellVolumeVerticalPillars(cell_idx);
		}

		// Normalize volumes to obtain relative volumes:
		double subsamplevolume = std::accumulate(rockvolumessubsample.begin(),
							 rockvolumessubsample.end(), 0.0);
		std::vector<double> rockvolumessubsample_normalized;
		for (size_t satnum_idx = 0; satnum_idx < rockvolumessubsample.size(); ++satnum_idx) {
		    rockvolumessubsample_normalized.push_back(rockvolumessubsample[satnum_idx]/subsamplevolume);
		}
		rockvolumes.push_back(rockvolumessubsample_normalized);
	    }

	    finished_subsamples++;
        }
        catch (...) {
            std::cerr << "Warning: Upscaling chopped subsample nr. " << sample << " failed, proceeding to next subsample\n";
        }
    }
    

    // Make stream of output data, to be outputted to screen and optionally to file
    std::stringstream outputtmp;

    outputtmp << "################################################################################################" << std::endl;
    outputtmp << "# Results from property analysis on subsamples" << std::endl;
    outputtmp << "#" << std::endl;
    time_t now = time(NULL);
    outputtmp << "# Finished: " << asctime(localtime(&now));

    utsname hostname;   uname(&hostname);
    outputtmp << "# Hostname: " << hostname.nodename << std::endl;
    outputtmp << "#" << std::endl;
    outputtmp << "# Options used:" << std::endl;
    outputtmp << "#     gridfilename: " << gridfilename << std::endl;
    outputtmp << "#   i; min,len,max: " << imin << " " << ilen << " " << imax << std::endl;
    outputtmp << "#   j; min,len,max: " << jmin << " " << jlen << " " << jmax << std::endl;
    outputtmp << "#   z; min,len,max: " << zmin << " " << zlen << " " << zmax << std::endl;
    outputtmp << "#       subsamples: " << subsamples << std::endl;
    if (userseed == 0) {
        outputtmp << "#      (auto) seed: " << autoseed << std::endl;
    }
    else {
        outputtmp << "#    (manual) seed: " << userseed << std::endl; 
    }        
    outputtmp << "################################################################################################" << std::endl;
    outputtmp << "# id";
    if (upscale) {
        if (isPeriodic) {
            if (ch.hasNTG()) {
                outputtmp << "          porosity                netporosity             ntg                      permx                   permy                   permz                   permyz                  permxz                  permxy                  netpermh";
            }
            else {
                outputtmp << "          porosity                 permx                   permy                   permz                   permyz                  permxz                  permxy";
            }
        }
        else if (isFixed) {
            if (ch.hasNTG()) {
                outputtmp << "          porosity                netporosity             ntg                      permx                   permy                   permz                   netpermh";
            }
            else {
                outputtmp << "          porosity                 permx                   permy                   permz";
            }
        }
        if (ch.hasSWCR()) {
            outputtmp << "               swcr ";
        }
        if (ch.hasSOWCR()) {
            outputtmp << "               sowcr ";
        }
    }
    if (endpoints) {
        if (!upscale) {
            if (ch.hasNTG()) {
                outputtmp << "          porosity                netporosity            ntg";
            }
            else {
                outputtmp << "          porosity";
            }
        }
        outputtmp << "                  Swir                    Swor";
        if (cappres) {
            outputtmp << "                  Pc(Swir)                Pc2                     Pc3                     Pc4                     Pc(Swor)";            
        }
    }
    if (dips) {
        outputtmp << "                  dip                     azim(displacement:" << azimuthdisplacement << ")";
    }
    if (satnumvolumes) {
	for (int satnumidx = 0; satnumidx < maxSatnum; ++satnumidx) {
	    outputtmp << "               satnum_" << satnumidx+1;
	}
    }
    outputtmp << std::endl;

    const int fieldwidth = outputprecision + 8;
    for (int sample = 1; sample <= finished_subsamples; ++sample) {
	outputtmp << sample << '\t';
	if (upscale) {
	    outputtmp <<
		std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << porosities[sample-1] << '\t';
            if (ch.hasNTG()) {
		outputtmp << std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << netporosities[sample-1] << '\t' <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << ntgs[sample-1] << '\t';
            }
            outputtmp <<
		std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << permxs[sample-1] << '\t' <<
		std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << permys[sample-1] << '\t' <<
		std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << permzs[sample-1] << '\t';
            if (isPeriodic) {
                outputtmp <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << permyzs[sample-1] << '\t' <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << permxzs[sample-1] << '\t' <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << permxys[sample-1] << '\t';                
            }
            if (ch.hasNTG()) {
                outputtmp <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << (permxs[sample-1]+permys[sample-1])/(2.0*ntgs[sample-1]) << '\t';
            }
            if (ch.hasSWCR()) {
                outputtmp <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << swcrs[sample-1] << '\t';
            }
            if (ch.hasSOWCR()) {
                outputtmp <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << sowcrs[sample-1] << '\t';
            }
	}
	if (endpoints) {
            if (!upscale) {
                outputtmp <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << porosities[sample-1] << '\t';
                if (ch.hasNTG()) {
                    outputtmp <<
                        std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << netporosities[sample-1] << '\t' <<
                        std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << ntgs[sample-1] << '\t';
                }

            }
	    outputtmp <<
		std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << minsws[sample-1] << '\t' <<
		std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << maxsws[sample-1];
            if (cappres) {
                outputtmp <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << pcvalues[sample-1][0] << '\t' <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << pcvalues[sample-1][1] << '\t' <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << pcvalues[sample-1][2] << '\t' <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << pcvalues[sample-1][3] << '\t' <<
                    std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << pcvalues[sample-1][4];
            }
	}
	if (dips) {
	    outputtmp <<
		std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << dipangs[sample-1] << '\t' <<
		std::showpoint << std::setw(fieldwidth) << std::setprecision(outputprecision) << azimuths[sample-1];
	}
	if (satnumvolumes) {
	    rockvolumes[sample-1].resize(maxSatnum, 0.0);
	    for (int satnumidx = 0; satnumidx < maxSatnum; ++satnumidx) {
		outputtmp <<
		    std::showpoint << std::setw(fieldwidth) <<
		    std::setprecision(outputprecision) << rockvolumes[sample-1][satnumidx] << '\t';
	    }
	}
	outputtmp <<  std::endl;
    }

    if (resultfile != "") {
	std::cout << "Writing results to " << resultfile << std::endl;
	std::ofstream outfile;
	outfile.open(resultfile.c_str(), std::ios::out | std::ios::trunc);
	outfile << outputtmp.str();
            outfile.close();
    }



    std::cout << outputtmp.str();
}
catch (const std::exception &e) {
    std::cerr << "Program threw an exception: " << e.what() << "\n";
    throw;
}
