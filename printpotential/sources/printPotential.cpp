#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cmath>
#include <iomanip>

#include <sys/stat.h>

#include "printPotential.hpp"

static void error(std::string const & errorMessage) {
    std::cerr << errorMessage;
    exit(EXIT_FAILURE);
}

PotentialForLammps::PotentialForLammps(
            const std::string &inputFileName,
            const IpcType type) :
        ipcType{type} {
    // read input.in file
    std::ifstream inputFile(inputFileName);
    if(inputFile.fail())
        error("File input.in could not be opened. Aborting.\n");
    inputFile >> e_BB >> e_Bs1 >> e_Bs2;
    inputFile >> e_s1s1 >> e_s2s2 >> e_s1s2;
    inputFile >> e_min;
    inputFile >> firstPatchEccentricity >> firstPatchRadius;
    inputFile >> secndPatchEccentricity >> secndPatchRadius;
    inputFile >> fakeHScoefficient >> fakeHSexponent;
    inputFile >> samplingStep >> cutoffValue;
    inputFile.close();

    // patch geometry integrity check
    double firstDiameter = firstPatchEccentricity + firstPatchRadius;
    double secondDiameter = secndPatchEccentricity + secndPatchRadius;
    if (std::abs(firstDiameter - secondDiameter) >= 1e-10 )
        error("eccentricities and radii are not consistent!\n");

    fakeHSdiameter = 1.0;
    ipcRadius = firstPatchEccentricity + firstPatchRadius;
    interactionRange = 2*ipcRadius;

    computeSiteSitePotentials();
}

static double computeOmega(double Ra, double Rb, double rab) {
    // BKL paper, formula 18
    if ( rab > Ra+Rb )
        return 0.;
    else if ( rab <= std::fabs(Ra-Rb) )
        return 8.*std::pow(std::min(Ra,Rb),3);
    else {
        const double tempSum = (Ra*Ra-Rb*Rb)/(2.*rab);
        return 2.*( (2.*Ra+tempSum+rab/2.)*pow(Ra-tempSum-rab/2.,2)
                  + (2.*Rb-tempSum+rab/2.)*pow(Rb+tempSum-rab/2.,2) );
    }
}

static double computeOmegaRadialDerivative(double Ra, double Rb, double rab) {
    // BKL paper, derivative of formula 18
    if ( rab >= Ra+Rb || rab <= fabs(Ra-Rb) )
        return 0.;
    else {
        const double tempSum = (Ra*Ra-Rb*Rb)/(2.*rab);
        const double tempSumMinus = tempSum - rab/2.;
        const double tempSumPlus = tempSum + rab/2.;
        return (6./rab) * (tempSumMinus*(Ra - tempSumPlus)*(Ra + tempSumPlus) - tempSumPlus*(Rb - tempSumMinus)*(Rb + tempSumMinus) );
    }
}

void PotentialForLammps::computeSiteSitePotentials()
{
    const size_t potentialSteps = size_t( interactionRange/samplingStep ) + 2;

    uHS.resize(potentialSteps);
    uBB.resize(potentialSteps);
    uBs1.resize(potentialSteps);
    uBs2.resize(potentialSteps);
    us1s2.resize(potentialSteps);
    us1s1.resize(potentialSteps);
    us2s2.resize(potentialSteps);

    fHS.resize(potentialSteps);
    fBB.resize(potentialSteps);
    fBs1.resize(potentialSteps);
    fBs2.resize(potentialSteps);
    fs1s2.resize(potentialSteps);
    fs1s1.resize(potentialSteps);
    fs2s2.resize(potentialSteps);

    for ( size_t i = 0; i < potentialSteps; ++i)
    {
        const double r = i*samplingStep;
        uBB[i]   = (e_BB  /e_min) * computeOmega(ipcRadius, ipcRadius, r);
        uBs1[i]  = (e_Bs1 /e_min) * computeOmega(ipcRadius, firstPatchRadius,  r);
        uBs2[i]  = (e_Bs2 /e_min) * computeOmega(ipcRadius, secndPatchRadius,  r);
        us1s2[i] = (e_s1s2/e_min) * computeOmega(firstPatchRadius,  secndPatchRadius,  r);
        us2s2[i] = (e_s2s2/e_min) * computeOmega(secndPatchRadius,  secndPatchRadius,  r);
        us1s1[i] = (e_s1s1/e_min) * computeOmega(firstPatchRadius,  firstPatchRadius,  r);

        fBB[i]   = (e_BB  /e_min) * computeOmegaRadialDerivative(ipcRadius, ipcRadius, r);
        fBs1[i]  = (e_Bs1 /e_min) * computeOmegaRadialDerivative(ipcRadius, firstPatchRadius,  r);
        fBs2[i]  = (e_Bs2 /e_min) * computeOmegaRadialDerivative(ipcRadius, secndPatchRadius,  r);
        fs1s2[i] = (e_s1s2/e_min) * computeOmegaRadialDerivative(firstPatchRadius,  secndPatchRadius,  r);
        fs2s2[i] = (e_s2s2/e_min) * computeOmegaRadialDerivative(secndPatchRadius,  secndPatchRadius,  r);
        fs1s1[i] = (e_s1s1/e_min) * computeOmegaRadialDerivative(firstPatchRadius,  firstPatchRadius,  r);

        if ( r <= fakeHSdiameter )
        {
            // setting up a Fake Hard Sphere Core
            double rm = pow(r, -fakeHSexponent);
            uHS[i]   += fakeHScoefficient*((rm-2.)*rm+1.);
            fHS[i]   += -2.*fakeHSexponent*fakeHScoefficient*(rm-1.)*rm;
        }
    }
}

void PotentialForLammps::printLAMMPSpotentialsToFile(const std::string &outputDirName) {
    // create output directory
    const std::string makedir = "mkdir -p " + outputDirName;
    if(system(makedir.c_str()) != 0)
        error("Problem while creating the directory.\n");

    // prepare strings for defining file names
    std::string interactionType [6];
    interactionType[0] = "BB";
    interactionType[1] = "Bs1";
    interactionType[2] = "Bs2";
    interactionType[3] = "s1s2";
    interactionType[4] = "s1s1";
    interactionType[5] = "s2s2";

    const size_t potentialSteps = uHS.size();

    for (int type = 0; type < 6; ++type) {
        // create the output file
        std::string fileName = outputDirName + "/" + interactionType[type] + ".table";
        std::ofstream potentialOutputFile(fileName);
        potentialOutputFile << "# potentials for lammps\n\n"
            << interactionType[type] << "\nN " << potentialSteps-1 << "\n\n";
            // -1 because we don't print r=0
        potentialOutputFile << std::scientific << std::setprecision(6);

        for ( size_t i = 1; i < potentialSteps; ++i) {
            const double r = i*samplingStep;
            potentialOutputFile << i << '\t' << r << '\t';

            // compute force and potential depending on type and cutoff
            double printPotential, printForce;
            if( type == 0) {
                printPotential = ( uHS[i] + uBB[i] > cutoffValue )? cutoffValue : uHS[i] + uBB[i];
                printForce = ((fHS[i] + fBB[i])*r < -cutoffValue )? -cutoffValue : -(fHS[i] + fBB[i]);
            } else if ( type == 1) {
                printPotential = ( uBs1[i] > cutoffValue )? cutoffValue : uBs1[i];
                printForce = (fBs1[i]*r < -cutoffValue )? -cutoffValue : -fBs1[i];
            } else if ( type == 2) {
                printPotential = ( uBs2[i] > cutoffValue )? cutoffValue : uBs2[i];
                printForce = (fBs2[i]*r < -cutoffValue )? -cutoffValue : -fBs2[i];
            } else if ( type == 3) {
                printPotential = ( us1s2[i] > cutoffValue )? cutoffValue : us1s2[i];
                printForce = (fs1s2[i]*r < -cutoffValue )? -cutoffValue : -fs1s2[i];
            } else if ( type == 4) {
                printPotential = ( us1s1[i] > cutoffValue )? cutoffValue : us1s1[i];
                printForce = (fs1s1[i]*r < -cutoffValue )? -cutoffValue : -fs1s1[i];
            } else if ( type == 5) {
                printPotential = ( us2s2[i] > cutoffValue )? cutoffValue : us2s2[i];
                printForce = (fs2s2[i]*r < -cutoffValue )? -cutoffValue : -fs2s2[i];
            }
            // finally, you can print
            potentialOutputFile << printPotential << '\t' << printForce << '\n';
        }
        potentialOutputFile.close();
    }

}

void PotentialForLammps::printRadialPotentialsToFile(std::string const& outputDirName) {
    // create output directory
    const std::string dirName = outputDirName + "_radial_plots";
    if(mkdir(dirName.c_str(), 0777) != 0)
        error("Problem while creating the directory for radial potentials.\n");

    std::vector<std::string> plotOrientations;
    if (ipcType == IpcType::JANUS) {
        plotOrientations.push_back("JANUS_SS");
        plotOrientations.push_back("JANUS_SP");
        plotOrientations.push_back("JANUS_SE");
        plotOrientations.push_back("JANUS_EP");
        plotOrientations.push_back("JANUS_PP");
        plotOrientations.push_back("JANUS_EE");
    } else {
        plotOrientations.push_back("EE");
        plotOrientations.push_back("Ep1");
        plotOrientations.push_back("Ep2");
        plotOrientations.push_back("p1p2");
        plotOrientations.push_back("p1p1");
        plotOrientations.push_back("p2p2");
    }

    for (int type = 0; type < plotOrientations.size(); ++type) {
        // create the output file
        std::string fileName = dirName + '/' + plotOrientations[type] + ".dat";
        std::ofstream potentialOutputFile(fileName);
        potentialOutputFile << std::scientific << std::setprecision(6);

        for ( double r = 1.0; r < interactionRange; r += samplingStep) {
            potentialOutputFile << r << '\t';

            size_t iBB = size_t(r/samplingStep);
            size_t iBs1 = size_t(  (r - firstPatchEccentricity)/samplingStep  );
            size_t iBs2 = size_t(  (r - secndPatchEccentricity)/samplingStep  );
            // compute potential depending on type and cutoff
            double printPotential;
            if( type == 0) {
                printPotential = uHS[iBB] + uBB[iBB];
            } else if ( type == 1) {
                printPotential = uHS[iBB] + uBB[iBB] + uBs1[iBs1];
            } else if ( type == 2) {
                printPotential = uHS[iBB] + uBB[iBB] + uBs2[iBs2];
            } else if ( type == 3) {
                size_t is1s2 = size_t(  (r - firstPatchEccentricity - secndPatchEccentricity)/samplingStep  );
                printPotential = uHS[iBB] + uBB[iBB] + uBs1[iBs1] + uBs2[iBs2] + us1s2[is1s2];
            } else if ( type == 4) {
                size_t is1s1 = size_t(  (r - 2*firstPatchEccentricity)/samplingStep  );
                printPotential = uHS[iBB] + uBB[iBB] + uBs1[iBs1] + uBs2[iBs2] + us1s1[is1s1];
            } else if ( type == 5) {
                size_t is2s2 = size_t(  (r - 2*secndPatchEccentricity)/samplingStep  );
                printPotential = uHS[iBB] + uBB[iBB] + uBs1[iBs1] + uBs2[iBs2] + us2s2[is2s2];
            }
            // finally, you can print
            potentialOutputFile << printPotential << '\n';
        }
        potentialOutputFile.close();
    }

}

size_t PotentialForLammps::dist(double x, double y) {
    double distance = std::sqrt(x*x + y*y);
    // std::cout << "dist = (" << x << ", " << y << ")\n";
    size_t distanceTab = size_t(distance/samplingStep);
    if (distanceTab > uHS.size()) {
        return 0;
    }
    return distanceTab;
}

static void log(std::string orient, size_t dist, double pot) {
    // std::cout << orient << " dist: " << dist*1e-5 << ", pot: " << pot << "\n";
}

void PotentialForLammps::printAngularPotentialsToFile(std::string const& outputDirName) {
    // create output directory
    const std::string dirName = outputDirName + "_angular_plots";
    if(mkdir(dirName.c_str(), 0777) != 0)
        error("Problem while creating the directory.\n");

    struct Orientation {
        std::string name;
        int theta_1;
        int theta_2;
    };
    std::vector<Orientation> plotOrientations;
    if (ipcType == IpcType::JANUS) {
        plotOrientations.push_back({"EE", 0, 180});
        plotOrientations.push_back({"PP", 180, 0});
        plotOrientations.push_back({"EP", 0, 0});
    } else {
        plotOrientations.push_back({"E",  90, 90});
        plotOrientations.push_back({"P1", 180, 0});
        plotOrientations.push_back({"P2", 0, 180});
    }

    for (auto& type: plotOrientations) {
        // create the output file
        std::string fileName = dirName + '/' + type.name + ".dat";
        std::ofstream potentialOutputFile(fileName);
        potentialOutputFile << std::scientific << std::setprecision(6);

        // set up the startingpoints
        double theta_1 = type.theta_1*M_PI/180.;
        double starting_theta_2 = type.theta_2;

        for (int angle = 0; angle < 360; angle += 5) {
            // compute potential depending on type and cutoff
            double theta_2 = (starting_theta_2 + angle)*M_PI/180.;
            double potential = 0;
            double dx = 0.;
            double dy = 0.;
            size_t distTab = 0;

            // CC
            distTab = dist(1.0, 0.0);
            if (distTab != 0) {
                double pot = uHS[distTab] + uBB[distTab];
                log("CC", distTab, pot);
                potential += pot;
            }
            // Cp1
            dx = 1.0 - firstPatchEccentricity*cos(theta_2);
            dy = firstPatchEccentricity*sin(theta_2);
            distTab = dist(dx, dy);
            if (distTab != 0) {
                double pot = uBs1[distTab];
                log("Cp1", distTab, pot);
                potential += pot;
            }
            // Cp2
            dx = 1.0 + secndPatchEccentricity*cos(theta_2);
            dy = -secndPatchEccentricity*sin(theta_2);
            distTab = dist(dx, dy);
            if (distTab != 0) {
                double pot = uBs2[distTab];
                log("Cp2", distTab, pot);
                potential += pot;
            }

            // p1C
            dx = firstPatchEccentricity*cos(theta_1) + 1.0;
            dy = firstPatchEccentricity*sin(theta_1);
            distTab = dist(dx, dy);
            if (distTab != 0) {
                double pot = uBs1[distTab];
                log("p1C", distTab, pot);
                potential += pot;
            }
            // p1p1
            dx = firstPatchEccentricity*cos(theta_1) + 1.0 - firstPatchEccentricity*cos(theta_2);
            dy = firstPatchEccentricity*sin(theta_1) - firstPatchEccentricity*sin(theta_2);
            distTab = dist(dx, dy);
            if (distTab != 0) {
                double pot = us1s1[distTab];
                log("p1p1", distTab, pot);
                potential += pot;
            }
            // p1p2
            dx = firstPatchEccentricity*cos(theta_1) + 1.0 + secndPatchEccentricity*cos(theta_2);
            dy = firstPatchEccentricity*sin(theta_1) + secndPatchEccentricity*sin(theta_2);
            distTab = dist(dx, dy);
            if (distTab != 0) {
                double pot = us1s2[distTab];
                log("p1p2", distTab, pot);
                potential += pot;
            }

            // p2C
            dx = 1.0 - secndPatchEccentricity*cos(theta_1);
            dy = secndPatchEccentricity*sin(theta_1);
            distTab = dist(dx, dy);
            if (distTab != 0) {
                double pot = uBs2[distTab];
                log("p2C", distTab, pot);
                potential += pot;
            }
            // p2p1
            dx = 1.0 - secndPatchEccentricity*cos(theta_1) - firstPatchEccentricity*cos(theta_2);
            dy = secndPatchEccentricity*sin(theta_1) + firstPatchEccentricity*sin(theta_2);
            distTab = dist(dx, dy);
            if (distTab != 0) {
                double pot = us1s2[distTab];
                log("p2p1", distTab, pot);
                potential += pot;
            }
            // p2p2
            dx = 1.0 - secndPatchEccentricity*cos(theta_1) + secndPatchEccentricity*cos(theta_2);
            dy = secndPatchEccentricity*sin(theta_1) - secndPatchEccentricity*sin(theta_2);
            distTab = dist(dx, dy);
            if (distTab != 0) {
                double pot = us2s2[distTab];
                log("p2p2", distTab, pot);
                potential += pot;
            }

            // finally print it
            log("total", 0, potential);
            // std::cout << "\n\n\n";
            potentialOutputFile << angle << '\t' << potential << '\n';
        }
    }
}
