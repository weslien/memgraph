/*
 * Copyright (c) 2015-2016 "Neo Technology,"
 * Network Engine for Objects in Lund AB [http://neotechnology.com]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

parser grammar Cypher;

options { tokenVocab=CypherLexer; }

cypher : statement ';'? EOF ;

statement : query ;

query : cypherQuery
      | indexQuery
      | explainQuery
      | profileQuery
      | databaseInfoQuery
      | systemInfoQuery
      | constraintQuery
      ;

constraintQuery : ( CREATE | DROP ) CONSTRAINT ON constraint ;

constraint : '(' nodeName=variable ':' labelName ')' ASSERT EXISTS '(' constraintPropertyList ')'
           | '(' nodeName=variable ':' labelName ')' ASSERT constraintPropertyList IS UNIQUE
           | '(' nodeName=variable ':' labelName ')' ASSERT '(' constraintPropertyList ')' IS NODE KEY
           | '(' nodeName=variable ':' labelName ')' ASSERT variable propertyLookup IS TYPED typeConstraintType
           ;

constraintPropertyList : variable propertyLookup ( ',' variable propertyLookup )* ;

storageInfo : STORAGE INFO ;

activeUsersInfo : ACTIVE USERS INFO | ACTIVE USERS ;

licenseInfo : LICENSE INFO ;

indexInfo : INDEX INFO | INDEXES ;

constraintInfo : CONSTRAINT INFO | CONSTRAINTS ;

edgetypeInfo : EDGE_TYPES INFO ;

nodelabelInfo : NODE_LABELS INFO ;

metricsInfo : METRICS INFO | METRICS ;

vectorIndexInfo : VECTOR INDEX INFO | VECTOR INDEXES ;

buildInfo : BUILD INFO ;

databaseInfoQuery : SHOW ( indexInfo | constraintInfo | edgetypeInfo | nodelabelInfo | metricsInfo | vectorIndexInfo ) ;

systemInfoQuery : SHOW ( storageInfo | buildInfo | activeUsersInfo | licenseInfo ) ;

explainQuery : EXPLAIN cypherQuery ;

profileQuery : PROFILE cypherQuery ;

cypherQuery : singleQuery ( cypherUnion )* ( queryMemoryLimit )? ;

indexQuery : createIndex | dropIndex;

singleQuery : clause ( clause )* ;

cypherUnion : ( UNION ALL singleQuery )
            | ( UNION singleQuery )
            ;

clause : cypherMatch
       | unwind
       | merge
       | create
       | set
       | cypherDelete
       | remove
       | with
       | cypherReturn
       | callProcedure
       ;

cypherMatch : OPTIONAL? MATCH pattern where? ;

unwind : UNWIND expression AS variable ;

merge : MERGE patternPart ( mergeAction )* ;

mergeAction : ( ON MATCH set )
            | ( ON CREATE set )
            ;

create : CREATE pattern ;

set : SET setItem ( ',' setItem )* ;

setItem : ( propertyExpression '=' expression )
        | ( variable '=' expression )
        | ( variable '+=' expression )
        | ( variable nodeLabels )
        ;

cypherDelete : DETACH? DELETE expression ( ',' expression )* ;

remove : REMOVE removeItem ( ',' removeItem )* ;

removeItem : ( variable nodeLabels )
           | propertyExpression
           ;

with : WITH ( DISTINCT )? returnBody ( where )? ;

cypherReturn : RETURN ( DISTINCT )? returnBody ;

callProcedure : CALL procedureName '(' ( expression ( ',' expression )* )? ')' ( procedureMemoryLimit )? ( yieldProcedureResults )? ;

procedureName : symbolicName ( '.' symbolicName )* ;

yieldProcedureResults : YIELD ( '*' | ( procedureResult ( ',' procedureResult )* ) ) ;

memoryLimit : MEMORY ( UNLIMITED | LIMIT literal ( MB | KB ) ) ;

queryMemoryLimit : QUERY memoryLimit ;

procedureMemoryLimit : PROCEDURE memoryLimit ;

procedureResult : ( variable AS variable ) | variable ;

returnBody : returnItems ( order )? ( skip )? ( limit )? ;

returnItems : ( '*' ( ',' returnItem )* )
            | ( returnItem ( ',' returnItem )* )
            ;

returnItem : ( expression AS variable )
           | expression
           ;

order : ORDER BY sortItem ( ',' sortItem )* ;

skip : L_SKIP expression ;

limit : LIMIT ( expression | parameter ) ;

sortItem : expression ( ASCENDING | ASC | DESCENDING | DESC )? ;

where : WHERE expression ;

pattern : patternPart ( ',' patternPart )* ;

patternPart : ( variable '=' anonymousPatternPart )
            | anonymousPatternPart
            ;

anonymousPatternPart : patternElement ;

patternElement : ( nodePattern ( patternElementChain )* )
               | ( '(' patternElement ')' )
               ;

nodePattern : '(' ( variable )? ( nodeLabels | labelExpression )? ( properties )? ')' ;

patternElementChain : relationshipPattern nodePattern ;

relationshipPattern : ( leftArrowHead dash ( relationshipDetail )? dash rightArrowHead )
                    | ( leftArrowHead dash ( relationshipDetail )? dash )
                    | ( dash ( relationshipDetail )? dash rightArrowHead )
                    | ( dash ( relationshipDetail )? dash )
                    ;

leftArrowHead : '<' | LeftArrowHeadPart ;
rightArrowHead : '>' | RightArrowHeadPart ;
dash : '-' | DashPart ;

relationshipDetail : '[' ( name=variable )? ( relationshipTypes )? ( variableExpansion )?  properties ']'
                   | '[' ( name=variable )? ( relationshipTypes )? ( variableExpansion )? relationshipLambda ( total_weight=variable )? (relationshipLambda )? ']'
                   | '[' ( name=variable )? ( relationshipTypes )? ( variableExpansion )? (properties )* ( relationshipLambda total_weight=variable )? (relationshipLambda )? ']';

relationshipLambda: '(' traversed_edge=variable ',' traversed_node=variable ( ',' accumulated_path=variable )? ( ',' accumulated_weight=variable )? '|' expression ')';

variableExpansion : '*' (BFS | WSHORTEST | ALLSHORTEST)? ( expression )? ( '..' ( expression )? )? ;

properties : mapLiteral
           | parameter
           ;

relationshipTypes : ':' relTypeName ( '|' ':'? relTypeName )* ;

nodeLabels : nodeLabel ( nodeLabel )* ;

nodeLabel : ':' labelName ;

labelExpression: ':' symbolicName ( '|' symbolicName )+ ;

labelName : symbolicName
          | parameter
          | variable ( propertyLookup )+
          ;

relTypeName : symbolicName
            | parameter
            | variable ( propertyLookup )+
            ;

expression : expression12 ;

expression12 : expression11 ( OR expression11 )* ;

expression11 : expression10 ( XOR expression10 )* ;

expression10 : expression9 ( AND expression9 )* ;

expression9 : ( NOT )* expression8 ;

expression8 : expression7 ( partialComparisonExpression )* ;

expression7 : expression6 ( ( '+' expression6 ) | ( '-' expression6 ) )* ;

expression6 : expression5 ( ( '*' expression5 ) | ( '/' expression5 ) | ( '%' expression5 ) )* ;

expression5 : expression4 ( '^' expression4 )* ;

expression4 : ( ( '+' | '-' ) )* expression3a ;

expression3a : expression2a ( stringAndNullOperators )* ;

stringAndNullOperators : ( ( ( ( '=~' ) | ( IN ) | ( STARTS WITH ) | ( ENDS WITH ) | ( CONTAINS ) ) expression2a) | ( IS CYPHERNULL ) | ( IS NOT CYPHERNULL ) ) ;

expression2a : expression2b ( nodeLabels )? ;

expression2b : atom ( memberAccess )* ;

memberAccess : propertyLookup
             | listIndexingOrSlicing
             ;

listIndexingOrSlicing : ( '[' expression ']' )
                      | ( '[' lower_bound=expression? '..' upper_bound=expression? ']' )
                      ;

atom : listComprehension
     | literal
     | parameter
     | caseExpression
     | ( COUNT '(' '*' ')' )
     | patternComprehension
     | ( FILTER '(' filterExpression ')' )
     | ( EXTRACT '(' extractExpression ')' )
     | ( REDUCE '(' reduceExpression ')' )
     | ( COALESCE '(' expression ( ',' expression )* ')' )
     | ( ALL '(' filterExpression ')' )
     | ( ANY '(' filterExpression ')' )
     | ( NONE '(' filterExpression ')' )
     | ( SINGLE '(' filterExpression ')' )
     | ( EXISTS '(' existsExpression ')' )
     | ( EXISTS '{' existsSubquery '}' )
     | relationshipsPattern
     | parenthesizedExpression
     | functionInvocation
     | variable
     | enumValueAccess
     ;

literal : numberLiteral
        | StringLiteral
        | booleanLiteral
        | CYPHERNULL
        | mapLiteral
        | mapProjectionLiteral
        | listLiteral
        ;

booleanLiteral : TRUE
               | FALSE
               ;

listLiteral : '[' ( expression ( ',' expression )* )? ']' ;

partialComparisonExpression : ( '=' expression7 )
                            | ( '<>' expression7 )
                            | ( '!=' expression7 )
                            | ( '<' expression7 )
                            | ( '>' expression7 )
                            | ( '<=' expression7 )
                            | ( '>=' expression7 )
                            ;

parenthesizedExpression : '(' expression ')' ;

relationshipsPattern : nodePattern ( patternElementChain )+ ;

filterExpression : idInColl ( where )? ;

reduceExpression : accumulator=variable '=' initial=expression ',' idInColl '|' expression ;

extractExpression : idInColl '|' expression ;

existsExpression : forcePatternPart | .* ;

existsSubquery : forcePatternPart
               | cypherQuery
               ;

forcePatternPart : ( variable '=' relationshipsPattern )
                 | relationshipsPattern
                 ;

idInColl : variable IN expression ;

functionInvocation : functionName '(' ( DISTINCT )? ( expression ( ',' expression )* )? ')' ;

functionName : symbolicName ( '.' symbolicName )* ;

listComprehension : '[' filterExpression ( '|' expression )? ']' ;

patternComprehension : '[' ( variable '=' )? relationshipsPattern ( where )? '|' resultExpr=expression ']' ;

propertyLookup : '.' ( propertyKeyName ) ;

allPropertiesLookup : '.' '*' ;

caseExpression : ( ( CASE ( caseAlternatives )+ ) | ( CASE test=expression ( caseAlternatives )+ ) ) ( ELSE else_expression=expression )? END ;

caseAlternatives : WHEN when_expression=expression THEN then_expression=expression ;

variable : symbolicName ;

numberLiteral : doubleLiteral
              | integerLiteral
              ;

mapLiteral : '{' ( propertyKeyName ':' expression ( ',' propertyKeyName ':' expression )* )? '}' ;

mapProjectionLiteral : variable '{' ( mapElement ( ',' mapElement )* )? '}' ;

mapElement : propertyLookup
           | allPropertiesLookup
           | variable
           | propertyKeyValuePair
           ;

parameter : '$' ( symbolicName | DecimalLiteral ) ;

propertyExpression : atom ( propertyLookup )+ ;

propertyKeyName : symbolicName ;

propertyKeyValuePair : propertyKeyName ':' expression ;

nestedPropertyKeyNames : propertyKeyName ( '.' propertyKeyName )* ;

integerLiteral : DecimalLiteral
               | OctalLiteral
               | HexadecimalLiteral
               ;

createIndex : CREATE INDEX ON ':' labelName ( '(' nestedPropertyKeyNames ( ',' nestedPropertyKeyNames )* ')' )? ;

dropIndex : DROP INDEX ON ':' labelName ( '(' nestedPropertyKeyNames ( ',' nestedPropertyKeyNames )* ')' )? ;

doubleLiteral : FloatingLiteral ;

enumValueAccess : symbolicName ':' ':' symbolicName ;

cypherKeyword : ALL
              | ALLSHORTEST
              | AND
              | ANY
              | AS
              | ASC
              | ASCENDING
              | ASSERT
              | BFS
              | BY
              | CALL
              | CASE
              | COALESCE
              | CONSTRAINT
              | CONTAINS
              | COUNT
              | CREATE
              | CYPHERNULL
              | DELETE
              | DESC
              | DESCENDING
              | DETACH
              | DISTINCT
              | DROP
              | ELSE
              | END
              | ENDS
              | EXISTS
              | EXPLAIN
              | EXTRACT
              | FALSE
              | FILTER
              | IN
              | INDEX
              | INFO
              | INSTANCE
              | IS
              | KB
              | KEY
              | L_SKIP
              | LIMIT
              | MATCH
              | MB
              | MEMORY
              | MERGE
              | NODE
              | NONE
              | NOT
              | ON
              | OPTIONAL
              | OR
              | ORDER
              | PROCEDURE
              | PROFILE
              | QUERY
              | REDUCE
              | REGISTER
              | REMOVE
              | RETURN
              | SET
              | SHOW
              | SINGLE
              | STARTS
              | STORAGE
              | THEN
              | TRUE
              | TYPED
              | UNION
              | UNIQUE
              | UNLIMITED
              | UNWIND
              | WHEN
              | WHERE
              | WITH
              | WSHORTEST
              | XOR
              | YIELD
              ;

symbolicName : UnescapedSymbolicName
             | EscapedSymbolicName
             | cypherKeyword
             | UNDERSCORE
             ;
