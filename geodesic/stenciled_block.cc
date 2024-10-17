/*!
\file stenciled_block.cc
\brief Implements the stenciled block class
\author Vladimir Florinski

This file is part of the SPECTRUM suite of scientific numerical simulation codes. SPECTRUM stands for Space Plasma and Energetic Charged particle TRansport on Unstructured Meshes. The code simulates plasma or neutral particle flows using MHD equations on a grid, transport of cosmic rays using stochastic or grid based methods. The "unstructured" part refers to the use of a geodesic mesh providing a uniform coverage of the surface of a sphere.
*/

#include <cmath>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include "common/polynomial.hh"
#include "geodesic/stenciled_block.hh"

namespace Spectrum {

//----------------------------------------------------------------------------------------------------------------------------------------------------
// StenciledBlock public methods
//----------------------------------------------------------------------------------------------------------------------------------------------------

/*!
\author Vladimir Florinski
\date 05/14/2024
*/
template <int verts_per_face>
StenciledBlock<verts_per_face>::~StenciledBlock()
{
   FreeStorage();
};

/*!
\author Vladimir Florinski
\date 05/17/2024
\param[in] width  Length of the side, without ghost cells
\param[in] wgohst Width of the ghost cell layer outside the sector
\param[in] height Hight of the block, without ghost cells
\param[in] hghost Number of ghost shells outside the slab
*/
template <int verts_per_face>
StenciledBlock<verts_per_face>::StenciledBlock(int width, int wghost, int height, int hghost)
                              : GridBlock<verts_per_face>(width, wghost, height, hghost)
{
   SetDimensions(width, wghost, height, hghost, true);
};

/*!
\author Vladimir Florinski
\date 05/17/2024
\param[in] width     Length of the side, without ghost cells
\param[in] wghost    Width of the ghost cell layer outside the sector
\param[in] height    Hight of the block, without ghost cells
\param[in] hghost    Number of ghost shells outside the slab
\param[in] construct Set to true when called from a constructor
*/
template <int verts_per_face>
void StenciledBlock<verts_per_face>::SetDimensions(int width, int wghost, int height, int hghost, bool construct)
{
// Call base method.
   if(!construct) GridBlock<verts_per_face>::SetDimensions(width, wghost, height, hghost, false);

   face_area = new double[n_faces_withghost];
   face_cmass = new GeoVector[n_faces_withghost];
   edge_length = new double[n_edges_withghost];

// Stenciled area is independent of the singular corners for 2nd order reconstructions, so the calculation can be done here. However, stencils themselves must be built after "AssociateMesh()". At this time each zonelist is set to "nullptr" so that the descructor works regardless of whether "AssociateMesh()" was invoked or not.
   MarkStenciledArea();
   stencil_zonelist = Create2D<int*>(n_faces_withghost, n_stencils);
   memset(stencil_zonelist[0], 0x0, n_faces_withghost * n_stencils * sizeof(int*));
   geom_matr_At = Create2D<Eigen::MatrixXd>(n_faces_withghost, n_stencils);
   geom_matr_LU = Create2D<Eigen::PartialPivLU<Eigen::MatrixXd>>(n_faces_withghost, n_stencils);
};

/*!
\author Vladimir Florinski
\date 05/14/2024
*/
template <int verts_per_face>
void StenciledBlock<verts_per_face>::FreeStorage(void)
{
   Delete2D(geom_matr_At);
   Delete2D(geom_matr_LU);

// Free up stencils
   for(auto pface = 0; pface < n_faces_withghost; pface++) {
      if(BITS_RAISED(face_mask[pface], GEOELM_STEN)) {
         for(auto stencil = 0; stencil < n_stencils; stencil++) {
            delete[] stencil_zonelist[pface][stencil];
         };
      };
   };
   Delete2D(stencil_zonelist);

// Free up storage for geometric properties
   delete[] face_area;
   delete[] face_cmass;
   delete[] edge_length;
};

/*!
\author Vladimir Florinski
\date 05/20/2024
\param[in] index       Unique ID of this block in the mesh
\param[in] ximin       Smallest reference distance of the block (without ghost)
\param[in] ximax       Largest reference distance of the block (without ghost)
\param[in] corners     Corner type, true for singular corners
\param[in] borders     Radial boundary type, true for external
\param[in] vcart       Vertex coordinate array in TAS/QAS
\param[in] dist_map_in Radial map function 
*/
template <int verts_per_face>
void StenciledBlock<verts_per_face>::AssociateMesh(int index, double ximin, double ximax, const bool* corners,
                                                   const bool* borders, const GeoVector* vcart, std::shared_ptr<DistanceBase> dist_map_in)
{
   GridBlock<verts_per_face>::AssociateMesh(index, ximin, ximax, corners, borders, vcart, dist_map_in);
   BuildAllStencils();

   ComputeMoments();
   for(auto pface = 0; pface < n_faces_withghost; pface++) {
      if(BITS_RAISED(face_mask[pface], GEOELM_STEN)) {
         for(auto stencil = 0; stencil < n_stencils; stencil++) {
            ComputeOneMatrix(pface, stencil);
         };
      };
   };
};

//----------------------------------------------------------------------------------------------------------------------------------------------------
// StenciledBlock protected methods
//----------------------------------------------------------------------------------------------------------------------------------------------------

/*!
\author Vladimir Florinski
\date 05/17/2024
*/
template <int verts_per_face>
void StenciledBlock<verts_per_face>::ComputeMoments(void)
{
   double area1, area2;
   GeoVector cm1, cm2;

// Compute face areas and face centers.
   for(auto face = 0; face < n_faces_withghost; face++) {
      if(BITS_RAISED(face_mask[face], GEOELM_NEXI)) {
         face_area[face] = 0.0;
         face_cmass[face] = gv_zeros;
         continue;
      };

// A triangle and its center of mass. The CM does not lie on the US!
      if(verts_per_face == 3) {
         area1 = SphTriArea(block_vert_cart[fv_local[face][0]], block_vert_cart[fv_local[face][1]], block_vert_cart[fv_local[face][2]]);
         area2 = 0.0;
         cm1 = SphTriCenter(block_vert_cart[fv_local[face][0]], block_vert_cart[fv_local[face][1]], block_vert_cart[fv_local[face][2]]);
         cm2 = gv_zeros;
      }

// Two triangles and the common center of mass
      else if(verts_per_face == 4) {
         area1 = SphTriArea(block_vert_cart[fv_local[face][0]], block_vert_cart[fv_local[face][1]], block_vert_cart[fv_local[face][2]]);
         area2 = SphTriArea(block_vert_cart[fv_local[face][2]], block_vert_cart[fv_local[face][3]], block_vert_cart[fv_local[face][0]]);
         cm1 = SphTriCenter(block_vert_cart[fv_local[face][0]], block_vert_cart[fv_local[face][1]], block_vert_cart[fv_local[face][2]]);
         cm2 = SphTriCenter(block_vert_cart[fv_local[face][2]], block_vert_cart[fv_local[face][3]], block_vert_cart[fv_local[face][0]]);
      };
      face_area[face] = area1 + area2;
      face_cmass[face] = (cm1 * area1 + cm2 * area2) / face_area[face];
   };

// Compute edge lengths
   for(auto edge = 0; edge < n_edges_withghost; edge++) {
      if(BITS_RAISED(edge_mask[edge], GEOELM_NEXI)) {
         edge_length[edge] = acos(block_vert_cart[ev_local[edge][0]] * block_vert_cart[ev_local[edge][1]]);
      };
   };
};

/*!
\author Vladimir Florinski
\date 05/19/2024
*/
template <int verts_per_face>
void StenciledBlock<verts_per_face>::MarkStenciledArea(void)
{
   int imax, jmax, i, j, face;
   std::pair<int, int> base_vert;

// TODO check if this works for HEX mesh
   imax = total_length - ghost_width;

// Mark the interior + one layer of faces
   base_vert = std::make_pair(square_fill * (ghost_width - 1), ghost_width - 1);
   for(i = square_fill * ghost_width; i <= imax; i++) {
      jmax = MaxFaceJ(base_vert, total_length - square_fill * ghost_width + 1, i);
      for(j = square_fill * (ghost_width - 1); j <= jmax; j++) {
         face = face_index_sector[i][j];
         RAISE_BITS(face_mask[face], GEOELM_STEN);
      };
   };

// Clip the small triangles at the SE corner.
   if(verts_per_face == 3) {
      base_vert = std::make_pair(total_length - ghost_width - 1, ghost_width - 1);
      for(i = imax - 1; i <= imax; i++) {
         jmax = MaxFaceJ(base_vert, 2, i);
         for(j = square_fill * (ghost_width - 1); j <= jmax; j++) {
            face = face_index_sector[i][j];
            LOWER_BITS(face_mask[face], GEOELM_STEN);
         };
      };

// Clip the small triangles at the N corner.
      base_vert = std::make_pair(total_length - ghost_width - 1, MaxVertJ(total_length, total_length - ghost_width - 1) - ghost_width + 1);
      for(i = imax - 1; i <= imax; i++) {
         jmax = MaxFaceJ(base_vert, 2, i);
         for(j = MaxFaceJ(base_vert, 2, imax - 1); j <= jmax; j++) {
            face = face_index_sector[i][j];
            LOWER_BITS(face_mask[face], GEOELM_STEN);
         };
      };
   };
};

/*!
\author Vladimir Florinski
\date 05/14/2024
*/
template <int verts_per_face>
void StenciledBlock<verts_per_face>::BuildAllStencils(void)
{
   int ic, nface, face;

   zones_per_stencil[0] = verts_per_face + 2;
   for(auto stencil = 1; stencil < n_stencils; stencil++) zones_per_stencil[stencil] = 4;

// Storage for stencilsets
   stencil_zonelist = Create2D<int*>(n_faces_withghost, n_stencils);
   for(auto pface = 0; pface < n_faces_withghost; pface++) {
      if(BITS_RAISED(face_mask[pface], GEOELM_STEN)) {
         for(auto stencil = 0; stencil < n_stencils; stencil++) {
            stencil_zonelist[pface][stencil] = new int[2 * zones_per_stencil[stencil]];
         };
      };
   };

//                                                 -----------
//          ---------------------                  |.........|                            -
//           \......./ \......./                   |.........|                           / \
//            \...../   \...../                    |.........|                          /   \
//             \.../ U+D \.../           ----------+---------+----------               /     \                         -----------
//              \./       \./            |.........|         |.........|              /       \                        |         |
//               -----------             |.........|   U+D   |.........|             -----------                       |         |
//                \......./              |.........|         |........ |            /.\......./.\                      |         |
//                 \...../               ----------+---------+----------           /...\.U/D./...\           ----------+---------+----------
//                  \.../                          |.........|                    /.....\.../.....\          |.........|.........|.........|
//                   \./                           |.........|                   /.......\./.......\         |.........|...U/D...|.........|
//                    -                            |.........|                  ---------------------        |.........|.........|.........|
//                                                 -----------                                               -------------------------------

// Calculate the zone lists
   for(auto pface = 0; pface < n_faces_withghost; pface++) {
      if(BITS_RAISED(face_mask[pface], GEOELM_STEN)) {

// Central
         for(ic = 0; ic < verts_per_face; ic++) {
            face = ff_local[pface][ic];
            stencil_zonelist[pface][0][2 * ic] = face; 
            stencil_zonelist[pface][0][2 * ic + 1] = 0;
         };
         stencil_zonelist[pface][0][2 * verts_per_face] = pface;
         stencil_zonelist[pface][0][2 * verts_per_face + 1] = -1;
         stencil_zonelist[pface][0][2 * verts_per_face + 2] = pface;
         stencil_zonelist[pface][0][2 * verts_per_face + 3] = 1;

// Directional
         for(auto stencil = 1; stencil <= verts_per_face; stencil++) {
            nface = ff_local[pface][stencil - 1];
            stencil_zonelist[pface][stencil][0] = stencil_zonelist[pface][stencil + verts_per_face][0] = nface;
            stencil_zonelist[pface][stencil][1] = stencil_zonelist[pface][stencil + verts_per_face][1] = 0;
            ic = InList(verts_per_face, ff_local[nface], pface);
            face = ff_local[nface][(ic + 1) % verts_per_face];
            stencil_zonelist[pface][stencil][2] = stencil_zonelist[pface][stencil + verts_per_face][2] = face;
            stencil_zonelist[pface][stencil][3] = stencil_zonelist[pface][stencil + verts_per_face][3] = 0;
            face = ff_local[nface][(ic + 4 - square_fill) % verts_per_face];
            stencil_zonelist[pface][stencil][4] = stencil_zonelist[pface][stencil + verts_per_face][4] = face;
            stencil_zonelist[pface][stencil][5] = stencil_zonelist[pface][stencil + verts_per_face][5] = 0;
            stencil_zonelist[pface][stencil][6] = stencil_zonelist[pface][stencil + verts_per_face][6] = nface;
            stencil_zonelist[pface][stencil][7] = -1;
            stencil_zonelist[pface][stencil + verts_per_face][7] = 1;
         };
      };
   };
};

/*!
\author Vladimir Florinski
\param[in] pface Principal face
\param[in] stencil Stencil (central, dir1, dir2, etc.)
*/
template <int verts_per_face>
void StenciledBlock<verts_per_face>::ComputeOneMatrix(int pface, int stencil)
{
   int face;
   double rp_factor;
   Eigen::MatrixXd geom_matr_A, geom_matr_AtA;
   geom_matr_A.resize(zones_per_stencil[stencil], 3);

// Generate the geometry matrix. Each row corresponds to one zone in the stencil.
   for(auto row = 0; row < zones_per_stencil[stencil]; row++) {
      face = stencil_zonelist[pface][stencil][2 * row];
      switch(stencil_zonelist[pface][stencil][2 * row + 1]) {
      case -1:
         rp_factor = (1.0 + drp_ratio);
         break;
      case 1:
         rp_factor = 1.0 / (1.0 + drp_ratio);
         break;
      default:
         rp_factor = 1.0;
         break;
      };
      for(auto col = 0; col < 3; col++) {
         geom_matr_A(row, col) = rp_factor * face_cmass[face][col] - face_cmass[pface][col];
      };
   };

// Compute AT, ATA, and the LU decomposition
   geom_matr_At[pface][stencil] = geom_matr_A.transpose();
   geom_matr_AtA = geom_matr_At[pface][stencil] * geom_matr_A;
   geom_matr_LU[pface][stencil].compute(geom_matr_AtA);
};

#ifdef GEO_DEBUG

/*!
\author Vladimir Florinski
\date 05/19/2024
\param[in] pface   Principal face
\param[in] stencil Stencil (central, dir1, dir2, etc.)
*/
template <int verts_per_face>
void StenciledBlock<verts_per_face>::PrintStencilProps(int pface, int stencil) const
{
   std::cerr << "Printing stencil " << stencil << " for principal face " << pface << std::endl;
   for(auto row = 0; row < zones_per_stencil[stencil]; row++) {
      std::cerr << "face: " << std::setw(5) << stencil_zonelist[pface][stencil][2 * row];
      std::cerr << ", plane: " << std::setw(5) << stencil_zonelist[pface][stencil][2 * row + 1];
      std::cerr << std::endl;
   };
   std::cerr << std::endl;
};

/*!
\author Vladimir Florinski
\date 05/19/2024
\param[in] k       Shell index
\param[in] pface   Principal face
\param[in] stencil Stencil (central, dir1, dir2, etc.)
\param[in] rot_z   The first rotation angle about the z-axis
\param[in] rot_x   The second rotation angle about the x-axis
*/
template <int verts_per_face>
void StenciledBlock<verts_per_face>:: DrawStencil(int k, int pface, int stencil, double rot_z, double rot_x) const
{
   int k1, face;

   for(auto row = 0; row < zones_per_stencil[stencil]; row++) {
      k1 = k + stencil_zonelist[pface][stencil][2 * row + 1];
      face = stencil_zonelist[pface][stencil][2 * row];
      GridBlock<verts_per_face>::DrawZone(k1, face, rot_z, rot_x);
   };
};

#endif

template class StenciledBlock<3>;
template class StenciledBlock<4>;

};
