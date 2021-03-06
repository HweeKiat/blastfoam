/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     | Website:  https://openfoam.org
    \\  /    A nd           | Copyright (C) 2011-2019 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
21-10-2019  Jeff Heylmun:   Added refinement to setFields utility
-------------------------------------------------------------------------------
License
    This file is derivative work of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

Description
    Set values on a selected set of cells/patchfaces through a dictionary and
    refines using hexRef method.

\*---------------------------------------------------------------------------*/

#include "argList.H"
#include "Time.H"
#include "fvMesh.H"
#include "topoSetSource.H"
#include "cellSet.H"
#include "faceSet.H"
#include "volFields.H"

#include "hexRef.H"
#include "hexRef8.H"
#include "mapPolyMesh.H"
#include "polyTopoChange.H"
#include "syncTools.H"
#include "wedgePolyPatch.H"
#include "errorEstimator.H"

#include "IOobjectList.H"

using namespace Foam;

#include "refineFunctions.H"

template<class Type>
bool setCellFieldType
(
    const word& fieldTypeDesc,
    const fvMesh& mesh,
    const labelList& selectedCells,
    Istream& fieldValueStream,
    const bool write
)
{
    typedef GeometricField<Type, fvPatchField, volMesh> fieldType;

    bool useOrig = false;
    if (fieldTypeDesc == fieldType::typeName + "InitialValue")
    {
        useOrig = true;
    }
    else if (fieldTypeDesc != fieldType::typeName + "Value")
    {
        return false;
    }

    word fieldName(fieldValueStream);
    word fieldOrigName(fieldName + "Orig");
    fieldType* field = 0;
    if (mesh.foundObject<fieldType>(fieldName))
    {
        field = &mesh.lookupObjectRef<fieldType>(fieldName);
    }
    else
    {
        // Check the current time directory
        IOobject fieldHeader
        (
            fieldName,
            mesh.time().timeName(),
            mesh,
            IOobject::MUST_READ
        );

        // Check the "constant" directory
        if (!fieldHeader.typeHeaderOk<fieldType>(true))
        {
            fieldHeader = IOobject
            (
                fieldName,
                mesh.time().constant(),
                mesh,
                IOobject::MUST_READ
            );
        }

        // Check field exists
        if (fieldHeader.typeHeaderOk<fieldType>(true))
        {
            field = new fieldType(fieldHeader, mesh);
        }
    }
    if (field && useOrig)
    {
        Info<< "    Setting internal values of "
            << field->headerClassName()
            << " " << fieldName << " to initial values" << endl;
    }
    else if (field)
    {
        Info<< "    Setting internal values of "
            << field->headerClassName()
            << " " << fieldName << endl;
    }
    else // Field is not found, so exit
    {
        WarningInFunction
            << "Field " << fieldName << " not found" << endl;

        // Consume value
        (void)pTraits<Type>(fieldValueStream);

        return true;
    }

    if (useOrig)
    {
        fieldType* origFieldPtr = 0;
        if (mesh.foundObject<fieldType>(fieldOrigName))
        {
            origFieldPtr = &mesh.lookupObjectRef<fieldType>(fieldOrigName);
        }
        else
        {
            origFieldPtr = new fieldType(fieldOrigName, *field);
            origFieldPtr->store(origFieldPtr);
        }
        (*field) = (*origFieldPtr);
        return true;
    }

    const Type& value = pTraits<Type>(fieldValueStream);

    if (selectedCells.size() == field->size())
    {
        field->primitiveFieldRef() = value;
    }
    else
    {
        forAll(selectedCells, celli)
        {
            (*field)[selectedCells[celli]] = value;
        }
    }

    typename GeometricField<Type, fvPatchField, volMesh>::
        Boundary& fieldBf = field->boundaryFieldRef();

    forAll(field->boundaryField(), patchi)
    {
        fieldBf[patchi] = fieldBf[patchi].patchInternalField();
    }

    if (field->name() != "error" && write)
    {
        if (!field->write())
        {
            FatalErrorInFunction
                << "Failed writing field " << field->name() << exit(FatalError);
        }
    }

    return true;
}


class setCellField
{

public:

    setCellField()
    {}

    autoPtr<setCellField> clone() const
    {
        return autoPtr<setCellField>(new setCellField());
    }

    class iNew
    {
        const fvMesh& mesh_;
        const labelList& selectedCells_;
        const bool write_;

    public:

        iNew(const fvMesh& mesh, const labelList& selectedCells, const bool write)
        :
            mesh_(mesh),
            selectedCells_(selectedCells),
            write_(write)
        {}

        autoPtr<setCellField> operator()(Istream& fieldValues) const
        {
            word fieldType(fieldValues);

            if
            (
               !(
                    setCellFieldType<scalar>
                        (fieldType, mesh_, selectedCells_, fieldValues, write_)
                 || setCellFieldType<vector>
                        (fieldType, mesh_, selectedCells_, fieldValues, write_)
                 || setCellFieldType<sphericalTensor>
                        (fieldType, mesh_, selectedCells_, fieldValues, write_)
                 || setCellFieldType<symmTensor>
                        (fieldType, mesh_, selectedCells_, fieldValues, write_)
                 || setCellFieldType<tensor>
                        (fieldType, mesh_, selectedCells_, fieldValues, write_)
                )
            )
            {
                WarningInFunction
                    << "field type " << fieldType << " not currently supported"
                    << endl;
            }

            return autoPtr<setCellField>(new setCellField());
        }
    };
};


template<class Type>
bool setFaceFieldType
(
    const word& fieldTypeDesc,
    const fvMesh& mesh,
    const labelList& selectedFaces,
    Istream& fieldValueStream,
    const bool write
)
{
    typedef GeometricField<Type, fvPatchField, volMesh> fieldType;

    if (fieldTypeDesc != fieldType::typeName + "Value")
    {
        return false;
    }

    word fieldName(fieldValueStream);
    fieldType* field = 0;
    if (mesh.foundObject<fieldType>(fieldName))
    {
        field =  &mesh.lookupObjectRef<fieldType>(fieldName);
    }
    else
    {
        // Check the current time directory
        IOobject fieldHeader
        (
            fieldName,
            mesh.time().timeName(),
            mesh,
            IOobject::MUST_READ
        );

        // Check the "constant" directory
        if (!fieldHeader.typeHeaderOk<fieldType>(true))
        {
            fieldHeader = IOobject
            (
                fieldName,
                mesh.time().constant(),
                mesh,
                IOobject::MUST_READ
            );
        }
    }
    if (field)
    {
        Info<< "    Setting internal values of "
            << field->headerClassName()
            << " " << fieldName << endl;
    }
    else
    {
        WarningInFunction
            << "Field " << fieldName << " not found" << endl;

        // Consume value
        (void)pTraits<Type>(fieldValueStream);
    }

    Info<< "    Setting patchField values of "
        << field->headerClassName()
        << " " << fieldName << endl;

    const Type& value = pTraits<Type>(fieldValueStream);

    // Create flat list of selected faces and their value.
    Field<Type> allBoundaryValues(mesh.nFaces()-mesh.nInternalFaces());
    forAll(field->boundaryField(), patchi)
    {
        SubField<Type>
        (
            allBoundaryValues,
            field->boundaryField()[patchi].size(),
            field->boundaryField()[patchi].patch().start()
          - mesh.nInternalFaces()
        ) = field->boundaryField()[patchi];
    }

    // Override
    bool hasWarned = false;
    labelList nChanged
    (
        returnReduce(field->boundaryField().size(), maxOp<label>()),
        0
    );
    forAll(selectedFaces, i)
    {
        label facei = selectedFaces[i];
        if (mesh.isInternalFace(facei))
        {
            if (!hasWarned)
            {
                hasWarned = true;
                WarningInFunction
                    << "Ignoring internal face " << facei
                    << ". Suppressing further warnings." << endl;
            }
        }
        else
        {
            label bFacei = facei-mesh.nInternalFaces();
            allBoundaryValues[bFacei] = value;
            nChanged[mesh.boundaryMesh().patchID()[bFacei]]++;
        }
    }

    Pstream::listCombineGather(nChanged, plusEqOp<label>());
    Pstream::listCombineScatter(nChanged);

    typename GeometricField<Type, fvPatchField, volMesh>::
        Boundary& fieldBf = field->boundaryFieldRef();

    // Reassign.
    forAll(field->boundaryField(), patchi)
    {
        if (nChanged[patchi] > 0)
        {
            Info<< "    On patch "
                << field->boundaryField()[patchi].patch().name()
                << " set " << nChanged[patchi] << " values" << endl;
            fieldBf[patchi] == SubField<Type>
            (
                allBoundaryValues,
                fieldBf[patchi].size(),
                fieldBf[patchi].patch().start()
              - mesh.nInternalFaces()
            );
        }
    }

    if (field->name() != "error" && write)
    {
        if (!field->write())
        {
            FatalErrorInFunction
                << "Failed writing field " << field->name() << exit(FatalError);
        }
    }

    return true;
}


class setFaceField
{

public:

    setFaceField()
    {}

    autoPtr<setFaceField> clone() const
    {
        return autoPtr<setFaceField>(new setFaceField());
    }

    class iNew
    {
        const fvMesh& mesh_;
        const labelList& selectedFaces_;
        const bool write_;

    public:

        iNew(const fvMesh& mesh, const labelList& selectedFaces, const bool write)
        :
            mesh_(mesh),
            selectedFaces_(selectedFaces),
            write_(write)
        {}

        autoPtr<setFaceField> operator()(Istream& fieldValues) const
        {
            word fieldType(fieldValues);

            if
            (
               !(
                    setFaceFieldType<scalar>
                        (fieldType, mesh_, selectedFaces_, fieldValues, write_)
                 || setFaceFieldType<vector>
                        (fieldType, mesh_, selectedFaces_, fieldValues, write_)
                 || setFaceFieldType<sphericalTensor>
                        (fieldType, mesh_, selectedFaces_, fieldValues, write_)
                 || setFaceFieldType<symmTensor>
                        (fieldType, mesh_, selectedFaces_, fieldValues, write_)
                 || setFaceFieldType<tensor>
                        (fieldType, mesh_, selectedFaces_, fieldValues, write_)
                )
            )
            {
                WarningInFunction
                    << "field type " << fieldType << " not currently supported"
                    << endl;
            }

            return autoPtr<setFaceField>(new setFaceField());
        }
    };
};


//- Read and add fields to the database
template<class FieldType>
void readAndAddFields(const fvMesh& mesh, const IOobjectList& objects)
{

    IOobjectList fields = objects.lookupClass(FieldType::typeName);
    forAllIter(IOobjectList, fields, fieldIter)
    {
        if (!mesh.foundObject<FieldType>(fieldIter()->name()))
        {
            IOobject fieldTargetIOobject
            (
                fieldIter()->name(),
                mesh.time().timeName(),
                mesh,
                IOobject::MUST_READ,
                IOobject::AUTO_WRITE
            );

            if (fieldTargetIOobject.typeHeaderOk<FieldType>(true))
            {
                FieldType* fPtr
                (
                    new FieldType
                    (
                        fieldTargetIOobject,
                        mesh
                    )
                );
                fPtr->store(fPtr);
            }
        }
    }
}


//- Read and add all fields to the database
void readAndAddAllFields(const fvMesh& mesh)
{
    // Get all fields present at the current time
    IOobjectList objects(mesh, mesh.time().timeName());

    readAndAddFields<volScalarField>(mesh, objects);
    readAndAddFields<volVectorField>(mesh, objects);
    readAndAddFields<volSphericalTensorField>(mesh, objects);
    readAndAddFields<volSymmTensorField>(mesh, objects);
    readAndAddFields<volTensorField>(mesh, objects);
}

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

int main(int argc, char *argv[])
{
    argList::addBoolOption
    (
        "updateAll",
        "Update size of all fields in the current time step"
    );
    argList::addBoolOption
    (
        "debug",
        "Output partial updates and additional fields"
    );
    argList::addBoolOption
    (
        "force3D",
        "Force 3-D refinement"
    );

    #include "addDictOption.H"
    #include "addRegionOption.H"
    #include "setRootCase.H"
    #include "createTime.H"
    #include "createNamedMesh.H"

    const word dictName("setFieldsDict");
    #include "setSystemMeshDictionaryIO.H"

    // Update All fields in the current time folder
    // Resizes to make sure all fields are consistent with the mesh
    bool updateAll(args.optionFound("updateAll"));

    Info<< "Reading " << dictName << "\n" << endl;

    IOdictionary setFieldsDict(dictIO);

    autoPtr<hexRef> meshCutter;
    if (args.optionFound("force3D"))
    {
        meshCutter.set(new hexRef8(mesh));
    }
    else
    {
        meshCutter = hexRef::New(mesh);
    }
    Switch debug(args.optionFound("debug"));

    label nBufferLayers(setFieldsDict.lookupOrDefault("nBufferLayers", 0));

    PackedBoolList protectedCell(mesh.nCells(), 0);
    initialize(mesh, protectedCell, meshCutter());
    volScalarField error
    (
        IOobject
        (
            "error",
            runTime.timeName(),
            mesh
        ),
        mesh,
        0.0
    );
    wordList fieldNames(setFieldsDict.lookupOrDefault("fields", wordList()));
    PtrList<volScalarField> fields(fieldNames.size());
    label fi = 0;
    forAll(fields, fieldi)
    {
        // Check the current time directory
        IOobject fieldHeader
        (
            fieldNames[fieldi],
            runTime.timeName(),
            mesh,
            IOobject::MUST_READ
        );

        // Check the "constant" directory
        if (fieldHeader.typeHeaderOk<volScalarField>(true))
        {
            fields.set
            (
                fi++,
                new volScalarField
                (
                    IOobject
                    (
                        fieldNames[fieldi],
                        runTime.timeName(),
                        mesh,
                        IOobject::MUST_READ,
                        IOobject::AUTO_WRITE
                    ),
                    mesh
                )
            );
        }
        else
        {
            WarningInFunction
                << "Field " << fieldNames[fieldi] << " specified for "
                << "setting refinement was not found. " << endl;
        }
    }
    fields.resize(fi);

    // Read in all fields to allow resizing
    if (updateAll)
    {
        readAndAddAllFields(mesh);
    }

    // Regions to refine based on a field
    PtrList<entry> regions(setFieldsDict.lookup("regions"));

    //- List of sources (and backups if present)
    // Stored to reduce the number of reads
    PtrList<topoSetSource> sources(regions.size());
    PtrList<topoSetSource> backupSources(regions.size());
    labelList levels(regions.size(), 0);
    forAll(regions, regionI)
    {
        sources.set
        (
            regionI,
            topoSetSource::New
            (
                regions[regionI].keyword(),
                mesh,
                regions[regionI].dict()
            ).ptr()
        );
        if (regions[regionI].dict().found("backup"))
        {
            backupSources.set
            (
                regionI,
                topoSetSource::New
                (
                    regions[regionI].keyword(),
                    mesh,
                    regions[regionI].dict().subDict("backup")
                ).ptr()
            );
        }
        levels[regionI] =
            regions[regionI].dict().lookupOrDefault("level", 0);
    }
    label maxRefinement
    (
        levels.size() && !setFieldsDict.found("maxLevel")
      ? max(levels)
      : setFieldsDict.lookupType<scalar>("maxLevel")
    );

    // Error fields is the same since it is looked up
    autoPtr<errorEstimator> EE;
    if (setFieldsDict.found("errorEstimator"))
    {
        EE = errorEstimator::New(mesh, setFieldsDict);
    }

    // Maximum number of iterations
    label iter = 0;
    label maxIter =
        max
        (
            1,
            max
            (
                setFieldsDict.lookupOrDefault("maxIter", 2*maxRefinement),
                gMax(meshCutter->cellLevel())*2
            )
        );

    // Flag for final iteration
    bool end = false;

    // Flag to initiate end
    bool prepareToStop = false;

    while(!end)
    {
        if (maxIter <= iter)
        {
            prepareToStop = true;
        }

        error = -1.0;

        // Check if this is the final iteration so correct cell shapes are used
        if (prepareToStop)
        {
            end = true;
        }

        // Read default values and set fields
        if (setFieldsDict.found("defaultFieldValues"))
        {
            Info<< "Setting field default values" << endl;
            PtrList<setCellField> defaultFieldValues
            (
                setFieldsDict.lookup("defaultFieldValues"),
                setCellField::iNew(mesh, labelList(mesh.nCells()), end)
            );
            Info<< endl;
        }

        // List of saved cells (per cell set)
        labelListList savedCells;

        Info<< "Setting field region values" << endl;
        forAll(regions, regionI)
        {
            // Cell sets
            if (sources[regionI].setType() == topoSetSource::CELLSETSOURCE)
            {
                cellSet selectedCellSet
                (
                    mesh,
                    "cellSet",
                    mesh.nCells()/10+1  // Reasonable size estimate.
                );

                sources[regionI].applyToSet
                (
                    topoSetSource::NEW,
                    selectedCellSet
                );

                labelList cells = selectedCellSet.toc();

                // Set to the actual cell set if at least one cells is found
                if
                (
                    returnReduce
                    (
                        selectedCellSet.toc().size(), sumOp<label>()
                    ) > 0
                )
                {
                    cells = selectedCellSet.toc();
                }
                // Use the backup region to expand search area
                else if (!end && backupSources.set(regionI))
                {
                    cellSet backupCellSet
                    (
                        mesh,
                        "cellSet",
                        mesh.nCells()/10+1  // Reasonable size estimate.
                    );

                    backupSources[regionI].applyToSet
                    (
                        topoSetSource::NEW,
                        backupCellSet
                    );
                    cells = backupCellSet.toc();
                }

                // Set field values
                if (regions[regionI].dict().found("fieldValues"))
                {
                    PtrList<setCellField> fieldValues
                    (
                        regions[regionI].dict().lookup("fieldValues"),
                        setCellField::iNew(mesh, cells, end || debug)
                    );
                }

                // Save the number of cells in the set
                savedCells.append(cells);

            }
            // Face sets
            else if (sources[regionI].setType() == topoSetSource::FACESETSOURCE)
            {
                faceSet selectedFaceSet
                (
                    mesh,
                    "faceSet",
                    (mesh.nFaces()-mesh.nInternalFaces())/10+1
                );

                sources[regionI].applyToSet
                (
                    topoSetSource::NEW,
                    selectedFaceSet
                );

                labelList selectedFaces(selectedFaceSet.toc());
                if (regions[regionI].dict().found("fieldValues"))
                {
                    PtrList<setFaceField> fieldValues
                    (
                        regions[regionI].dict().lookup("fieldValues"),
                        setFaceField::iNew(mesh, selectedFaces, end || debug)
                    );
                }

                // Set owner and neighbor cells of the selected faces
                labelList faceCells(selectedFaces.size()*2);
                label nFaces = 0;
                forAll(selectedFaces, i)
                {
                    label facei = selectedFaces[i];
                    faceCells[nFaces] = mesh.faceOwner()[facei];
                    nFaces++;
                    if (mesh.isInternalFace(facei))
                    {
                        faceCells[nFaces] = mesh.faceNeighbour()[facei];
                        nFaces++;
                    }
                }
                faceCells.resize(nFaces);
                savedCells.append(faceCells);
            }
        }

        // Update boundary conditions of fields used for refinement
        forAll(fields, fieldi)
        {
            fields[fieldi].correctBoundaryConditions();
        }

        // Update error and mesh if not the final iteration
        if (!end)
        {
            // Use error estimator to calculate error
            if (EE.valid())
            {
                EE->update();
            }
            // Check for any difference
            else
            {
                calcFaceDiff(error, fields);
            }
            labelList maxCellLevel(mesh.nCells(), -1);
            forAll(regions, regionI)
            {
                // Set keyword to lookup for refinement
                word refineKeyword;
                if (sources[regionI].setType() == topoSetSource::CELLSETSOURCE)
                {
                    refineKeyword = "refineInternal";
                }
                else
                {
                    refineKeyword = "refineFaces";
                }

                // Do not use the max level, use current
                // Order is important in the definitions of regions
                Switch overwrite =
                    regions[regionI].dict().lookupOrDefault<Switch>
                    (
                        "overwriteLevel",
                        false
                    );

                // Set actual max cell level
                forAll(savedCells[regionI], celli)
                {
                    if (overwrite)
                    {
                        maxCellLevel[savedCells[regionI][celli]] = levels[regionI];
                    }
                    else
                    {
                        maxCellLevel[savedCells[regionI][celli]] =
                            max
                            (
                                maxCellLevel[savedCells[regionI][celli]],
                                levels[regionI]
                            );
                    }
                }

                // Set specified cells to be refined
                if
                (
                    regions[regionI].dict().lookupOrDefault<Switch>
                    (
                        refineKeyword,
                        false
                    ) && !EE.valid()
                )
                {
                    forAll(savedCells[regionI], celli)
                    {
                        error[savedCells[regionI][celli]] = 1.0;
                    }
                }

                // Extend refinement by nBufferLayers
                for (label i = 0; i < nBufferLayers; i++)
                {
                    extendMaxCellLevel
                    (
                        mesh,
                        savedCells[regionI],
                        maxCellLevel,
                        levels[regionI],
                        i == 0
                    );
                }
            }

            // Set the maxCell level
            forAll(maxCellLevel, celli)
            {
                if (maxCellLevel[celli] < 0)
                {
                    maxCellLevel[celli] = maxRefinement;
                }
            }

            // Mark cells greater than the max cell level for unrefinment
            forAll(error, celli)
            {
                if (meshCutter->cellLevel()[celli] > maxCellLevel[celli])
                {
                    error[celli] = -1.0;
                }
            }

            // Write fields and mesh if using debug
            if (debug)
            {
                bool writeOk = (mesh.write() && meshCutter->write());
                volScalarField scalarCellLevel
                (
                    IOobject
                    (
                        "cellLevel",
                        runTime.timeName(),
                        mesh,
                        IOobject::NO_READ,
                        IOobject::AUTO_WRITE,
                        false
                    ),
                    mesh,
                    dimensionedScalar(dimless, 0)
                );
                volScalarField scalarMaxCellLevel
                (
                    IOobject
                    (
                        "maxCellLevel",
                        runTime.timeName(),
                        mesh,
                        IOobject::NO_READ,
                        IOobject::AUTO_WRITE,
                        false
                    ),
                    mesh,
                    dimensionedScalar(dimless, 0)
                );

                const labelList& cellLevel = meshCutter->cellLevel();

                forAll(cellLevel, celli)
                {
                    scalarCellLevel[celli] = cellLevel[celli];
                    scalarMaxCellLevel[celli] = maxCellLevel[celli];
                }
                writeOk = writeOk && scalarCellLevel.write();
                error.write();
                scalarMaxCellLevel.write();
                runTime++;
            }

            // Update mesh (return if mesh changes)
            prepareToStop =
               !update
                (
                    mesh,
                    error,
                    setFieldsDict,
                    meshCutter(),
                    protectedCell,
                    maxCellLevel
                );
            // Force refinement data to go to the current time directory.
            meshCutter->setInstance(runTime.timeName());
        }
        iter++;
    }

    // Write all fields
    if (updateAll)
    {
        runTime.write();
    }

    // Handle cell level (as volScalarField) explicitly
    // Important when using mapFields or rotateFields
    const labelIOList& cellLevel = meshCutter->cellLevel();
    if (mesh.foundObject<volScalarField>("cellLevel"))
    {
        volScalarField& scalarCellLevel =
            mesh.lookupObjectRef<volScalarField>("cellLevel");
        forAll(cellLevel, celli)
        {
            scalarCellLevel[celli] = cellLevel[celli];
        }
        scalarCellLevel.write();
    }
    else
    {
        volScalarField scalarCellLevel
        (
            IOobject
            (
                "cellLevel",
                runTime.timeName(),
                mesh
            ),
            mesh,
            dimensionedScalar(dimless, 0)
        );
        forAll(cellLevel, celli)
        {
            scalarCellLevel[celli] = cellLevel[celli];
        }
        scalarCellLevel.write();
    }

    // Write mesh and cell levels
    mesh.write();
    meshCutter->write();

    Info<< "\nEnd\n" << endl;

    return 0;
}


// ************************************************************************* //
