function x = gb_get_scalar (A)
%GB_GET_SCALAR get the first scalar from a matrix

% SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2020, All Rights
% Reserved. http://suitesparse.com.  See GraphBLAS/Doc/License.txt.

% TODO

[~, ~, x] = GrB.extracttuples (A) ;
if (isempty (x))
    x = 0 ;
else
    x = x (1) ;
end

