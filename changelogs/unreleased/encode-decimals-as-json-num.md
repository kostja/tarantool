## bugfix/sql
* Add `encode_decimal_as_number` parameter to JSON config.
That forces to encode `decimal` as JSON number to force type consistency in JSON output.
Use with catious - most of JSON parsers assume that number is restricted to float64.
