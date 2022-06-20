## bugfix/sql

Fixes a failing inner join query, projecting parameters from the inner table to the result projection. As a side effect the default column metadata was changed from `boolean` to `any` type.
