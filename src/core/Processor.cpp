/*
Copyright (c) 2003-2010 Sony Pictures Imageworks Inc., et al.
All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Sony Pictures Imageworks nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <OpenColorIO/OpenColorIO.h>

#include "GpuShaderUtils.h"
#include "HashUtils.h"
#include "LogOps.h"
#include "Lut3DOp.h"
#include "MatrixOps.h"
#include "OpBuilders.h"
#include "Processor.h"
#include "ScanlineHelper.h"

#include <cstring>
#include <sstream>

#include <iostream>

OCIO_NAMESPACE_ENTER
{
    //////////////////////////////////////////////////////////////////////////
    
    
    
    Processor::~Processor()
    { }
    
    
    
    //////////////////////////////////////////////////////////////////////////
    
    
    
    namespace
    {
        void BuildAllocationOps(OpRcPtrVec & ops,
                                const AllocationData & data,
                                TransformDirection dir)
        {
            if(data.allocation == ALLOCATION_UNIFORM)
            {
                float oldmin[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                float oldmax[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                float newmin[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                float newmax[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                
                if(data.vars.size() >= 2)
                {
                    for(int i=0; i<3; ++i)
                    {
                        oldmin[i] = data.vars[0];
                        oldmax[i] = data.vars[1];
                    }
                }
                
                CreateFitOp(ops,
                            oldmin, oldmax,
                            newmin, newmax,
                            dir);
            }
            else if(data.allocation == ALLOCATION_LG2)
            {
                float oldmin[4] = { -10.0f, -10.0f, -10.0f, 0.0f };
                float oldmax[4] = { 6.0f, 6.0f, 6.0f, 1.0f };
                float newmin[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                float newmax[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                
                if(data.vars.size() >= 2)
                {
                    for(int i=0; i<3; ++i)
                    {
                        oldmin[i] = data.vars[0];
                        oldmax[i] = data.vars[1];
                    }
                }
                
                
                // Log Settings
                // output = k * log(mx+b, base) + kb
                
                float k[3] = { 1.0f, 1.0f, 1.0f };
                float m[3] = { 1.0f, 1.0f, 1.0f };
                float b[3] = { 0.0f, 0.0f, 0.0f };
                float base[3] = { 2.0f, 2.0f, 2.0f };
                float kb[3] = { 0.0f, 0.0f, 0.0f };
                
                if(data.vars.size() >= 3)
                {
                    for(int i=0; i<3; ++i)
                    {
                        b[i] = data.vars[2];
                    }
                }
                
                if(dir == TRANSFORM_DIR_FORWARD)
                {
                    CreateLogOp(ops, k, m, b, base, kb, dir);
                    
                    CreateFitOp(ops,
                                oldmin, oldmax,
                                newmin, newmax,
                                dir);
                }
                else if(dir == TRANSFORM_DIR_INVERSE)
                {
                    CreateFitOp(ops,
                                oldmin, oldmax,
                                newmin, newmax,
                                dir);
                    
                    CreateLogOp(ops, k, m, b, base, kb, dir);
                }
                else
                {
                    throw Exception("Cannot BuildAllocationOps, unspecified transform direction.");
                }
            }
            else
            {
                throw Exception("Unsupported Allocation Type.");
            }
        }
        
        void WriteShaderHeader(std::ostringstream & shader, const std::string & pixelName,
                               const GpuShaderDesc & shaderDesc)
        {
            if(!shader) return;
            
            std::string lut3dName = "lut3d";
            
            shader << "\n// Generated by OpenColorIO\n\n";
            
            GpuLanguage lang = shaderDesc.getLanguage();
            
            std::string fcnName = shaderDesc.getFunctionName();
            
            if(lang == GPU_LANGUAGE_CG)
            {
                shader << "half4 " << fcnName << "(in half4 inPixel," << "\n";
            }
            else if(lang == GPU_LANGUAGE_GLSL_1_0 || lang == GPU_LANGUAGE_GLSL_1_3)
            {
                shader << "vec4 " << fcnName << "(in vec4 inPixel, \n";
            }
            else throw Exception("Unsupported shader language.");
            
            shader << "    const uniform sampler3D " << lut3dName << ") \n";
            shader << "{" << "\n";
            
            if(lang == GPU_LANGUAGE_CG)
            {
                shader << "half4 " << pixelName << " = inPixel; \n";
            }
            else if(lang == GPU_LANGUAGE_GLSL_1_0 || lang == GPU_LANGUAGE_GLSL_1_3)
            {
                shader << "vec4 " << pixelName << " = inPixel; \n";
            }
            else throw Exception("Unsupported shader language.");
        }
        
        
        void WriteShaderFooter(std::ostringstream & shader,
                               const std::string & pixelName,
                               const GpuShaderDesc & /*shaderDesc*/)
        {
            shader << "return " << pixelName << ";\n";
            shader << "}" << "\n\n";
        }
        
        // Find the minimal index range in the opVec that does not support
        // shader text generation.  The endIndex *is* inclusive.
        // 
        // I.e., if the entire opVec does not support GPUShaders, the
        // result will be startIndex = 0, endIndex = opVec.size() - 1
        // 
        // If the entire opVec supports GPU generation, both the
        // startIndex and endIndex will equal -1
        
        void GetGpuUnsupportedIndexRange(int * startIndex, int * endIndex,
                                         const OpRcPtrVec & opVec)
        {
            int start = -1;
            int end = -1;
            
            for(unsigned int i=0; i<opVec.size(); ++i)
            {
                // We've found a gpu unsupported op.
                // If it's the first, save it as our start.
                // Otherwise, update the end.
                
                if(!opVec[i]->supportsGpuShader())
                {
                    if(start<0)
                    {
                        start = i;
                        end = i;
                    }
                    else end = i;
                }
            }
            
            // Now that we've found a startIndex, walk back until we find
            // one that defines a GpuAllocation. (we can only upload to
            // the gpu at a location are tagged with an allocation)
            
            while(start>0)
            {
                if(opVec[start]->definesAllocation()) break;
                 --start;
            }
            
            if(startIndex) *startIndex = start;
            if(endIndex) *endIndex = end;
        }
        
        AllocationData GetAllocation(int index, const OpRcPtrVec & opVec)
        {
            if(index >=0 && opVec[index]->definesAllocation())
            {
                return opVec[index]->getAllocation();
            }
            
            return AllocationData();
        }
    }
    
    
    //////////////////////////////////////////////////////////////////////////
    
    
    LocalProcessorRcPtr LocalProcessor::Create()
    {
        return LocalProcessorRcPtr(new LocalProcessor(), &deleter);
    }
    
    void LocalProcessor::deleter(LocalProcessor* p)
    {
        delete p;
    }
    
    LocalProcessor::LocalProcessor()
    { }
    
    LocalProcessor::~LocalProcessor()
    { }
    
    void LocalProcessor::addColorSpaceConversion(const Config & config,
                                 const ConstColorSpaceRcPtr & srcColorSpace,
                                 const ConstColorSpaceRcPtr & dstColorSpace)
    {
        BuildColorSpaceOps(m_cpuOps, config, srcColorSpace, dstColorSpace);
    }
    
    
    void LocalProcessor::addTransform(const Config & config,
                      const ConstTransformRcPtr& transform,
                      TransformDirection direction)
    {
        BuildOps(m_cpuOps, config, transform, direction);
    }
    
    void LocalProcessor::finalize()
    {
        // GPU Process setup
        {
            //
            // Partition the original, raw opvec into 3 segments for GPU Processing
            //
            // Interior index range does not support the gpu shader.
            // This is used to bound our analytical shader text generation
            // start index and end index are inclusive.
            
            int gpuLut3DOpStartIndex = 0;
            int gpuLut3DOpEndIndex = 0;
            GetGpuUnsupportedIndexRange(&gpuLut3DOpStartIndex,
                                        &gpuLut3DOpEndIndex,
                                        m_cpuOps);
            
            // Write the entire shader using only shader text (3d lut is unused)
            if(gpuLut3DOpStartIndex == -1 && gpuLut3DOpEndIndex == -1)
            {
                for(unsigned int i=0; i<m_cpuOps.size(); ++i)
                {
                    m_gpuOpsHwPreProcess.push_back( m_cpuOps[i]->clone() );
                }
            }
            // Analytical -> 3dlut -> analytical
            else
            {
                // Handle analytical shader block before start index.
                for(int i=0; i<gpuLut3DOpStartIndex; ++i)
                {
                    m_gpuOpsHwPreProcess.push_back( m_cpuOps[i]->clone() );
                }
                
                // Get the GPU Allocation at the cross-over point
                // Create 2 symmetrically canceling allocation ops,
                // where the shader text moves to a nicely allocated LDR
                // (low dynamic range color space), and the lattice processing
                // does the inverse (making the overall operation a no-op
                // color-wise
                
                AllocationData allocation = GetAllocation(gpuLut3DOpStartIndex, m_cpuOps);
                BuildAllocationOps(m_gpuOpsHwPreProcess, allocation, TRANSFORM_DIR_FORWARD);
                BuildAllocationOps(m_gpuOpsCpuLatticeProcess, allocation, TRANSFORM_DIR_INVERSE);
                
                // Handle cpu lattice processing
                for(int i=gpuLut3DOpStartIndex; i<=gpuLut3DOpEndIndex; ++i)
                {
                    m_gpuOpsCpuLatticeProcess.push_back( m_cpuOps[i]->clone() );
                }
                
                // And then handle the gpu post processing
                for(int i=gpuLut3DOpEndIndex+1; i<(int)m_cpuOps.size(); ++i)
                {
                    m_gpuOpsHwPostProcess.push_back( m_cpuOps[i]->clone() );
                }
            }
            
            // TODO: Optimize opvecs
            FinalizeOpVec(m_gpuOpsHwPreProcess);
            FinalizeOpVec(m_gpuOpsCpuLatticeProcess);
            FinalizeOpVec(m_gpuOpsHwPostProcess);
        }
        
        // CPU Process setup
        {
            // TODO: Optimize opvec
            FinalizeOpVec(m_cpuOps);
        }
        
        /*
        std::cerr << "     ********* CPU OPS ***************" << std::endl;
        std::cerr << GetOpVecInfo(m_cpuOps) << "\n\n";
        
        std::cerr << "     ********* GPU OPS PRE PROCESS ***************" << std::endl;
        std::cerr << GetOpVecInfo(m_gpuOpsHwPreProcess) << "\n\n";
        
        std::cerr << "     ********* GPU OPS LATTICE PROCESS ***************" << std::endl;
        std::cerr << GetOpVecInfo(m_gpuOpsCpuLatticeProcess) << "\n\n";
        
        std::cerr << "     ********* GPU OPS POST PROCESS ***************" << std::endl;
        std::cerr << GetOpVecInfo(m_gpuOpsHwPostProcess) << "\n\n";
        */
    }
    
    
    bool LocalProcessor::isNoOp() const
    {
        return IsOpVecNoOp(m_cpuOps);
    }
    
    void LocalProcessor::apply(ImageDesc& img) const
    {
        if(m_cpuOps.empty()) return;
        
        ScanlineHelper scanlineHelper(img);
        float * rgbaBuffer = 0;
        long numPixels = 0;
        
        while(true)
        {
            scanlineHelper.prepRGBAScanline(&rgbaBuffer, &numPixels);
            if(numPixels == 0) break;
            if(!rgbaBuffer)
                throw Exception("Cannot apply transform; null image.");
            
            for(OpRcPtrVec::size_type i=0, size = m_cpuOps.size(); i<size; ++i)
            {
                m_cpuOps[i]->apply(rgbaBuffer, numPixels);
            }
            
            scanlineHelper.finishRGBAScanline();
        }
    }
    
    void LocalProcessor::applyRGB(float * pixel) const
    {
        if(m_cpuOps.empty()) return;
        
        // We need to allocate a temp array as the pixel must be 4 floats in size
        // (otherwise, sse loads will potentially fail)
        
        float rgbaBuffer[4] = { pixel[0], pixel[1], pixel[2], 0.0f };
        
        for(OpRcPtrVec::size_type i=0, size = m_cpuOps.size(); i<size; ++i)
        {
            m_cpuOps[i]->apply(rgbaBuffer, 1);
        }
        
        pixel[0] = rgbaBuffer[0];
        pixel[1] = rgbaBuffer[1];
        pixel[2] = rgbaBuffer[2];
    }
    
    void LocalProcessor::applyRGBA(float * pixel) const
    {
        for(OpRcPtrVec::size_type i=0, size = m_cpuOps.size(); i<size; ++i)
        {
            m_cpuOps[i]->apply(pixel, 1);
        }
    }
    
    
    
    
    
    
    
    
    ///////////////////////////////////////////////////////////////////////////
    
    const char * LocalProcessor::getGpuShaderText(const GpuShaderDesc & shaderDesc) const
    {
        std::ostringstream shader;
        std::string pixelName = "out_pixel";
        std::string lut3dName = "lut3d";
        
        WriteShaderHeader(shader, pixelName, shaderDesc);
        
        
        for(unsigned int i=0; i<m_gpuOpsHwPreProcess.size(); ++i)
        {
            m_gpuOpsHwPreProcess[i]->writeGpuShader(shader, pixelName, shaderDesc);
        }
        
        if(!m_gpuOpsCpuLatticeProcess.empty())
        {
            // Sample the 3D LUT.
            int lut3DEdgeLen = shaderDesc.getLut3DEdgeLen();
            shader << pixelName << ".rgb = ";
            Write_sampleLut3D_rgb(&shader, pixelName,
                                  lut3dName, lut3DEdgeLen,
                                  shaderDesc.getLanguage());
        }
        
        for(unsigned int i=0; i<m_gpuOpsHwPostProcess.size(); ++i)
        {
            m_gpuOpsHwPostProcess[i]->writeGpuShader(shader, pixelName, shaderDesc);
        }
        
        WriteShaderFooter(shader, pixelName, shaderDesc);
        
        // TODO: This is not multi-thread safe. Cache result or mutex
        m_shaderText = shader.str();
        
        return m_shaderText.c_str();
    }
    
    const char * LocalProcessor::getGpuLut3DCacheID(const GpuShaderDesc & shaderDesc) const
    {
        // Can we write the entire shader using only shader text?
        // Lut3D is not needed. Blank it.
        
        if(m_gpuOpsCpuLatticeProcess.empty())
        {
            // TODO: This is not multi-thread safe. Cache result or mutex
            m_lut3DHash = "<NULL>";
            return m_lut3DHash.c_str();
        }
        
        // For all ops that will contribute to the 3D lut,
        // add it to the hash
        
        std::ostringstream idhash;
        
        // Apply the lattice ops to the cacheid
        for(int i=0; i<(int)m_gpuOpsCpuLatticeProcess.size(); ++i)
        {
            idhash << m_gpuOpsCpuLatticeProcess[i]->getCacheID() << " ";
        }
        
        // Also, add a hash of the shader description
        idhash << shaderDesc.getLanguage() << " ";
        idhash << shaderDesc.getFunctionName() << " ";
        idhash << shaderDesc.getLut3DEdgeLen() << " ";
        std::string fullstr = idhash.str();
        
        // TODO: This is not multi-thread safe. Cache result or mutex
        m_lut3DHash = CacheIDHash(fullstr.c_str(), (int)fullstr.size());
        return m_lut3DHash.c_str();
    }
    
    void LocalProcessor::getGpuLut3D(float* lut3d, const GpuShaderDesc & shaderDesc) const
    {
        if(!lut3d) return;
        
        // Can we write the entire shader using only shader text?
        // Lut3D is not needed. Blank it.
        
        int lut3DEdgeLen = shaderDesc.getLut3DEdgeLen();
        int lut3DNumPixels = lut3DEdgeLen*lut3DEdgeLen*lut3DEdgeLen;
        
        if(m_gpuOpsCpuLatticeProcess.empty())
        {
            memset(lut3d, 0, sizeof(float) * 3 * lut3DNumPixels);
            return;
        }
        
        // Allocate rgba 3dlut image
        float lut3DRGBABuffer[lut3DNumPixels*4];
        GenerateIdentityLut3D(lut3DRGBABuffer, lut3DEdgeLen, 4);
        
        // Apply the lattice ops to it
        for(int i=0; i<(int)m_gpuOpsCpuLatticeProcess.size(); ++i)
        {
            m_gpuOpsCpuLatticeProcess[i]->apply(lut3DRGBABuffer, lut3DNumPixels);
        }
        
        // Copy the lut3d rgba image to the lut3d
        for(int i=0; i<lut3DNumPixels; ++i)
        {
            lut3d[3*i+0] = lut3DRGBABuffer[4*i+0];
            lut3d[3*i+1] = lut3DRGBABuffer[4*i+1];
            lut3d[3*i+2] = lut3DRGBABuffer[4*i+2];
        }
    }
}
OCIO_NAMESPACE_EXIT
