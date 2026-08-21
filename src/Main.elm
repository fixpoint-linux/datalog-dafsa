module Main exposing (main)

{-| The datalog-dafsa documentation site as a plain Browser.element app.
This module renders all 9 content pages using the shared Fixpoint.* design package.
-}

import Browser
import Fixpoint.Callout
import Fixpoint.Checks
import Fixpoint.Code
import Fixpoint.Cta
import Fixpoint.Footer
import Fixpoint.Headline
import Fixpoint.Hero
import Fixpoint.Nav
import Fixpoint.Style
import Html exposing (Html, a, b, code, div, em, h1, h2, h3, li, nav, ol, p, pre, span, strong, table, tbody, td, text, th, thead, tr, ul)
import Html.Attributes exposing (attribute, class, href, id)


main : Program Flags Model Msg
main =
    Browser.element
        { init = init
        , update = update
        , view = view
        , subscriptions = subscriptions
        }


type alias Flags = { pathname : String }

type Page = Landing | Language | Cli | Api | Architecture | TimeTravel | VectorSearch | OrderStatistics | TypedProjects

type alias Model = Page

type Msg = NoOp


init : Flags -> (Model, Cmd Msg)
init flags =
    ( parsePage (stripDafsaPrefix flags.pathname), Cmd.none )


{-| Strip a leading `/datalog-dafsa` prefix and any surrounding slashes so the
result is the bare sub-page slug (e.g. `"/datalog-dafsa/language/"` ->
`"language"`, `"/datalog-dafsa/"` -> `""`). Falls back to `""` for `/`.
-}
stripDafsaPrefix : String -> String
stripDafsaPrefix raw =
    let
        withoutPrefix =
            if String.startsWith "/datalog-dafsa" raw then
                String.dropLeft (String.length "/datalog-dafsa") raw

            else if raw == "/" then
                ""

            else
                raw
    in
    withoutPrefix
        |> String.dropLeft (if String.startsWith "/" withoutPrefix then 1 else 0)
        |> (\s -> if String.endsWith "/" s then String.dropRight 1 s else s)


parsePage : String -> Page
parsePage path =
    case path of
        "" -> Landing
        "language" -> Language
        "cli" -> Cli
        "api" -> Api
        "architecture" -> Architecture
        "time-travel" -> TimeTravel
        "vector-search" -> VectorSearch
        "order-statistics" -> OrderStatistics
        "typed-projects" -> TypedProjects
        _ -> Landing


update : Msg -> Model -> (Model, Cmd Msg)
update _ model = (model, Cmd.none)


subscriptions : Model -> Sub Msg
subscriptions _ = Sub.none


view : Model -> Html Msg
view model =
    div [] [ Fixpoint.Style.stylesheet, navView, div [ class "wrap" ] [ pageView model ], footerView ]


navView : Html Msg
navView =
    Fixpoint.Nav.view
        { brand = span [] [ span [ class "fx" ] [ text "fx" ], text "://datalog-dafsa" ]
        , links =
            [ a [ href "https://fixpointlinux.org/datalog-dafsa/", attribute "data-mfe-route" "/datalog-dafsa" ] [ text "Overview" ]
            , a [ href "https://fixpointlinux.org/datalog-dafsa/language/", attribute "data-mfe-route" "/datalog-dafsa/language" ] [ text "Language" ]
            , a [ href "https://fixpointlinux.org/datalog-dafsa/cli/", attribute "data-mfe-route" "/datalog-dafsa/cli" ] [ text "CLI" ]
            , a [ href "https://fixpointlinux.org/datalog-dafsa/api/", attribute "data-mfe-route" "/datalog-dafsa/api" ] [ text "C API" ]
            , a [ href "https://fixpointlinux.org/datalog-dafsa/architecture/", attribute "data-mfe-route" "/datalog-dafsa/architecture" ] [ text "Architecture" ]
            , a [ href "https://fixpointlinux.org/datalog-dafsa/time-travel/", attribute "data-mfe-route" "/datalog-dafsa/time-travel" ] [ text "Time Travel" ]
            , a [ href "https://fixpointlinux.org/datalog-dafsa/vector-search/", attribute "data-mfe-route" "/datalog-dafsa/vector-search" ] [ text "Vector Search" ]
            , a [ href "https://fixpointlinux.org/datalog-dafsa/order-statistics/", attribute "data-mfe-route" "/datalog-dafsa/order-statistics" ] [ text "Order Stats" ]
            , a [ href "https://fixpointlinux.org/datalog-dafsa/typed-projects/", attribute "data-mfe-route" "/datalog-dafsa/typed-projects" ] [ text "Typed Projects" ]
            ]
        , extra =
            [ a [ class "home", href "https://fixpointlinux.org/datalog-dafsa/playground/", attribute "data-mfe-route" "/datalog-dafsa/playground" ] [ text "Playground" ]
            , a [ class "home", href "https://fixpointlinux.org/", attribute "data-mfe-route" "/" ]
                [ text "fixpoint-linux" ]
            ]
        }


pageView : Model -> Html Msg
pageView model =
    case model of
        Landing -> landingView
        Language -> languageView
        Cli -> cliView
        Api -> apiView
        Architecture -> architectureView
        TimeTravel -> timeTravelView
        VectorSearch -> vectorSearchView
        OrderStatistics -> orderStatisticsView
        TypedProjects -> typedProjectsView


footerView : Html Msg
footerView =
    Fixpoint.Footer.view
        [ text "Datalog engine source lives in "
        , a [ href "https://github.com/fixpoint-linux/datalog-dafsa" ] [ text "github.com/fixpoint-linux/datalog-dafsa" ]
        , Fixpoint.Footer.sep
        , text "part of "
        , a [ href "https://fixpointlinux.org" ] [ text "fixpoint-linux" ]
        ]


landingView : Html Msg
landingView =
    div []
        [ h1 [] [ text "A DAFSA-backed Datalog engine that ", Fixpoint.Hero.fx [ text "never forgets itself" ], text "." ]
        , p []
            [ text "Load facts into an on-disk "
            , strong [] [ text "minimal-acyclic-DAFSA" ]
            , text " store, compile Datalog rules to a small VM, materialize derived relations, and serve reads from an mmap’d snapshot. Every publish is an immutable, versioned point-in-time — so "
            , strong [] [ text "time travel is a first-class feature" ]
            , text ", not an afterthought."
            ]
        , Fixpoint.Cta.view
            { body =
                [ strong [] [ text "Try it right here" ]
                , text " — the real engine, compiled to WebAssembly, runs in your browser. No setup, no server."
                ]
            , href = "https://fixpointlinux.org/datalog-dafsa/playground/"
            , label = "Open the Playground →"
            , attrs = [ attribute "data-mfe-route" "/datalog-dafsa/playground" ]
            }
        , h2 [] [ text "The headline: time travel" ]
        , div [ class "hint" ] [ text "// dl_publish_snapshot · dl_snapshot_versions · dl_query_version" ]
        , p []
            [ text "Every "
            , Fixpoint.Code.inline "dl_publish_snapshot"
            , text " writes an "
            , strong [] [ text "immutable, versioned snapshot" ]
            , text " and keeps the full history by default. The database is "
            , strong [] [ text "content-addressed by construction" ]
            , text " — the timeline is its complete history. Read it as it was at any version with an "
            , em [] [ text "as-of" ]
            , text " query, diff or replay the evolution of a derived relation, and roll back — without ever losing the record of what happened."
            ]
        , div [ class "timeline" ]
            [ Fixpoint.Code.k "$"
            , text " ./dl -d db versions\n"
            , Fixpoint.Code.c "v042"
            , text " "
            , span [ class "dim" ] [ text "2026-08-18 09:12 · published · ok" ]
            , text "\n"
            , Fixpoint.Code.c "v041"
            , text " "
            , span [ class "dim" ] [ text "2026-08-17 22:04 · published · ok" ]
            , text "\n"
            , Fixpoint.Code.c "v040"
            , text " "
            , span [ class "dim" ] [ text "2026-08-17 18:55 · rolled-forward to v042" ]
            , text "\n...\n"
            , Fixpoint.Code.k "$"
            , text " ./dl -d db search 'gpu rental' --top 10 --version 39   "
            , span [ class "dim" ] [ text "# as-of a past snapshot" ]
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " ./dl -d db versions                                  "
            , span [ class "dim" ] [ text "# the full history" ]
            ]
        , Fixpoint.Checks.view
            [ li [] [ b [] [ text "Immutable snapshots" ], text " — later writes never disturb a published version." ]
            , li []
                [ b [] [ text "As-of queries" ]
                , text " — "
                , Fixpoint.Code.inline "dl_query_version"
                , text ", "
                , Fixpoint.Code.inline "dl_search_version"
                , text ", and "
                , Fixpoint.Code.inline "dl_vector_search_version"
                , text " read the exact past state."
                ]
            , li [] [ b [] [ text "Opt-in retention" ], text " — keep N snapshots, prune the rest, done." ]
            ]
        , p [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/time-travel/", attribute "data-mfe-route" "/datalog-dafsa/time-travel" ] [ text "The time-travel & as-of guide →" ] ]
        , h2 [] [ text "What else stands out" ]
        , Fixpoint.Headline.view
            [ Fixpoint.Headline.card
                { n = "01"
                , title = [ text "A database that ", em [] [ text "shares its suffixes" ], text " — genuinely compact" ]
                , body =
                    [ p []
                        [ text "Every relation is stored as a "
                        , strong [] [ text "minimized acyclic DAFSA" ]
                        , text ": common suffix paths between facts merge into a single shared state, not duplicated. Reads are "
                        , strong [] [ text "mmap’d zero-copy" ]
                        , text " — the DAFSA "
                        , em [] [ text "is" ]
                        , text " the index, so there’s no separate index file and no deserialization on the read path. Exact lookup and prefix enumeration are the two most common join access patterns, and both are native DAFSA primitives."
                        ]
                    , p [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/architecture/#compact", attribute "data-mfe-route" "/datalog-dafsa/architecture" ] [ text "Why the DAFSA makes the store small →" ] ]
                    ]
                }
            , Fixpoint.Headline.card
                { n = "02"
                , title = [ text "Typed projects — schema, validated data, typechecked rules" ]
                , body =
                    [ p []
                        [ text "The "
                        , Fixpoint.Code.inline "dlp"
                        , text " tool defines the schema in Dhall ("
                        , Fixpoint.Code.inline "schema.dhall"
                        , text "), validates and coerces CSV/JSON data against it, and "
                        , strong [] [ text "typechecks every rule" ]
                        , text " before compilation. Mixed-type rules are rejected with "
                        , Fixpoint.Code.inline "file:line:col"
                        , text " diagnostics — type errors surface early instead of mis-evaluating."
                        ]
                    , p [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/typed-projects/", attribute "data-mfe-route" "/datalog-dafsa/typed-projects" ] [ text "The typed project workflow →" ] ]
                    ]
                }
            , Fixpoint.Headline.card
                { n = "03"
                , title = [ text "Search — full-text ", em [] [ text "and" ], text " semantic, both versioned" ]
                , body =
                    [ p []
                        [ Fixpoint.Code.inline "dl search"
                        , text " is AND-intersect full-text over a postings index. The vector tier adds "
                        , strong [] [ text "semantic" ]
                        , text " retrieval ("
                        , Fixpoint.Code.inline "dl vsearch"
                        , text " / "
                        , Fixpoint.Code.inline "dl vhybrid"
                        , text "): bge-small embeddings via the "
                        , Fixpoint.Code.inline "dl-embed"
                        , text " tool, MIH-over-ITQ candidate retrieval, and in-store int8 re-rank — all stored in-relation and snapshot-versioned like everything else."
                        ]
                    , p [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/vector-search/", attribute "data-mfe-route" "/datalog-dafsa/vector-search" ] [ text "The vector-search tier →" ] ]
                    ]
                }
            , Fixpoint.Headline.card
                { n = "04"
                , title = [ text "Order statistics — rank, select, range, count" ]
                , body =
                    [ p []
                        [ text "The big-endian encoding means the "
                        , strong [] [ text "extreme prefix is the extreme key" ]
                        , text ", so "
                        , Fixpoint.Code.inline "rank"
                        , text "/"
                        , Fixpoint.Code.inline "select"
                        , text "/"
                        , Fixpoint.Code.inline "range_count"
                        , text "/"
                        , Fixpoint.Code.inline "count"
                        , text " fall out of the DAFSA naturally — with bound + permutation-index variants, a pull-iterator + merge-join, and a lazy range generator. Median, percentiles, and ordered scans are native primitives, not table scans."
                        ]
                    , p [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/order-statistics/", attribute "data-mfe-route" "/datalog-dafsa/order-statistics" ] [ text "The order-statistics guide →" ] ]
                    ]
                }
            ]
        , h2 [] [ text "Quickstart" ]
        , Fixpoint.Code.block
            [ text "make                 # build libdatalog.so, dl CLI, test binaries\n"
            , text "make test            # run the full test suite\n"
            , text "make bench           # run the demonstration benchmark"
            ]
        , p []
            [ text "The "
            , Fixpoint.Code.inline "dl"
            , text " CLI loads facts and answers queries. The database directory defaults to "
            , Fixpoint.Code.inline "dl-test-db"
            , text " and can be overridden with "
            , Fixpoint.Code.inline "-d <dir>"
            , text "."
            ]
        , Fixpoint.Code.block
            [ Fixpoint.Code.c "# Load a headerless CSV (arity 1-8) into a relation."
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " ./dl -d /tmp/db load edges.csv --rel edge\n"
            , Fixpoint.Code.g "Loaded 5 facts into edge"
            , text "\n\n"
            , Fixpoint.Code.c "# Exact lookup + prefix enumeration."
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " ./dl -d /tmp/db lookup edge 1 2\n"
            , Fixpoint.Code.g "found"
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " ./dl -d /tmp/db prefix edge 2\n"
            , Fixpoint.Code.g "2 3\n2 4"
            , text "\n\n"
            , Fixpoint.Code.c "# Transitive closure via a Datalog rule."
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " ./dl -d /tmp/db query 'tc(X,Y) :- edge(X,Y). tc(X,Y) :- edge(X,Z), tc(Z,Y).' tc\n"
            , Fixpoint.Code.g "1 2\n1 3\n1 4\n1 5\n2 3\n2 4\n2 5\n3 5"
            , text "\n\n"
            , Fixpoint.Code.c "# Publish a snapshot — an immutable point-in-time."
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " ./dl -d /tmp/db publish\n"
            , Fixpoint.Code.g "Snapshot published."
            ]
        , p []
            [ text "CSV values that parse as integers are stored raw as u32; anything else is interned to a symbol id. Other commands include "
            , Fixpoint.Code.inline "bound"
            , text ", "
            , Fixpoint.Code.inline "pattern"
            , text " (regex), "
            , Fixpoint.Code.inline "qmagic"
            , text " (magic-sets), "
            , Fixpoint.Code.inline "search"
            , text ", "
            , Fixpoint.Code.inline "vsearch"
            , text ", "
            , Fixpoint.Code.inline "vhybrid"
            , text ", and "
            , Fixpoint.Code.inline "versions"
            , text ". See "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/cli/", attribute "data-mfe-route" "/datalog-dafsa/cli" ] [ text "the CLI reference" ]
            , text "."
            ]
        , h2 [] [ text "Feature summary" ]
        , table [ class "features" ]
            [ thead []
                [ tr []
                    [ th [] [ text "Area" ]
                    , th [] [ text "Capabilities" ]
                    , th [] [ text "Details" ]
                    ]
                ]
            , tbody []
                [ tr []
                    [ td [ class "name" ] [ text "Time travel" ]
                    , td []
                        [ text "Immutable versioned snapshots, as-of queries ("
                        , Fixpoint.Code.inline "dl_query_version"
                        , text ", "
                        , Fixpoint.Code.inline "dl_search_version"
                        , text ", "
                        , Fixpoint.Code.inline "dl_vector_search_version"
                        , text "), opt-in retention"
                        ]
                    , td [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/time-travel/", attribute "data-mfe-route" "/datalog-dafsa/time-travel" ] [ text "Time Travel" ] ]
                    ]
                , tr []
                    [ td [ class "name" ] [ text "DAFSA storage" ]
                    , td [] [ text "Fixed-width u32BE key encoding; one DAFSA + WAL per relation; symbol interner; WAL + compaction; mmap zero-copy reads" ]
                    , td [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/architecture/", attribute "data-mfe-route" "/datalog-dafsa/architecture" ] [ text "Architecture" ] ]
                    ]
                , tr []
                    [ td [ class "name" ] [ text "Datalog syntax" ]
                    , td [] [ text "Facts, rules, recursion, negation, aggregates (count/sum/min/max), equality, comparisons, arithmetic, strings, lists, range, regex" ]
                    , td [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/language/", attribute "data-mfe-route" "/datalog-dafsa/language" ] [ text "Language Reference" ] ]
                    ]
                , tr []
                    [ td [ class "name" ] [ text "Evaluation" ]
                    , td [] [ text "Semi-naive fixpoint, stratified negation, bushy joins, permutation-index selection + hash-join, magic-sets / QSQ top-down, incremental view maintenance" ]
                    , td [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/architecture/#strategies", attribute "data-mfe-route" "/datalog-dafsa/architecture" ] [ text "Architecture § strategies" ] ]
                    ]
                , tr []
                    [ td [ class "name" ] [ text "Order statistics" ]
                    , td [] [ text "rank / select / range_count / count, bound + perm variants, pull-iterator + merge-join, lazy range generator" ]
                    , td [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/order-statistics/", attribute "data-mfe-route" "/datalog-dafsa/order-statistics" ] [ text "Order Statistics" ] ]
                    ]
                , tr []
                    [ td [ class "name" ] [ text "Durability" ]
                    , td [] [ text "Per-relation WAL + fsync, single-writer lock, atomic snapshot publish, mmap read path" ]
                    , td [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/architecture/#durability", attribute "data-mfe-route" "/datalog-dafsa/architecture" ] [ text "Architecture § durability" ] ]
                    ]
                , tr []
                    [ td [ class "name" ] [ text "Search" ]
                    , td []
                        [ text "Full-text "
                        , Fixpoint.Code.inline "dl search"
                        , text " + semantic "
                        , Fixpoint.Code.inline "dl vsearch"
                        , text "/"
                        , Fixpoint.Code.inline "dl vhybrid"
                        , text " (bge-small via "
                        , Fixpoint.Code.inline "dl-embed"
                        , text ", MIH-over-ITQ, in-store int8 re-rank), all versioned"
                        ]
                    , td [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/vector-search/", attribute "data-mfe-route" "/datalog-dafsa/vector-search" ] [ text "Vector Search" ] ]
                    ]
                ]
            ]
        , h2 [] [ text "Reference pages" ]
        , ul []
            [ li [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/language/", attribute "data-mfe-route" "/datalog-dafsa/language" ] [ text "Language Reference" ], text " — the complete rule language." ]
            , li [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/cli/", attribute "data-mfe-route" "/datalog-dafsa/cli" ] [ text "CLI Reference" ], text " — the ", Fixpoint.Code.inline "dl", text " subcommands." ]
            , li [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/api/", attribute "data-mfe-route" "/datalog-dafsa/api" ] [ text "C API Reference" ], text " — the full ", Fixpoint.Code.inline "dl.h", text " surface." ]
            , li [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/architecture/", attribute "data-mfe-route" "/datalog-dafsa/architecture" ] [ text "Architecture" ], text " — storage thesis, lifecycle, join & evaluation strategies." ]
            , li [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/time-travel/", attribute "data-mfe-route" "/datalog-dafsa/time-travel" ] [ text "Time Travel" ], text " — versioned snapshots and as-of queries." ]
            , li [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/vector-search/", attribute "data-mfe-route" "/datalog-dafsa/vector-search" ] [ text "Vector Search" ], text " — semantic ", Fixpoint.Code.inline "dl vsearch", text "/", Fixpoint.Code.inline "dl vhybrid", text ", bge-small via ", Fixpoint.Code.inline "dl-embed", text "." ]
            , li [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/order-statistics/", attribute "data-mfe-route" "/datalog-dafsa/order-statistics" ] [ text "Order Statistics" ], text " — rank / select / range / count and the sorted iterator." ]
            , li [] [ a [ href "https://fixpointlinux.org/datalog-dafsa/typed-projects/", attribute "data-mfe-route" "/datalog-dafsa/typed-projects" ] [ text "Typed Projects" ], text " — schema.dhall + validated data + typechecked rules." ]
            ]
        ]


languageView : Html Msg
languageView =
    div []
        [ h1 [] [ text "Language Reference" ]
        , p []
            [ text "This is the complete reference for the rule language. Every example is a real construct that the parser and compiler accept (several are drawn directly from the passing test suite). Rules are passed to the engine as source text via "
            , Fixpoint.Code.inline "dl_load_rules"
            , text ", the CLI "
            , Fixpoint.Code.inline "query"
            , text " / "
            , Fixpoint.Code.inline "qmagic"
            , text " commands, or a "
            , Fixpoint.Code.inline ".dl"
            , text " file."
            ]
        , h2 [ id "comments" ] [ text "Comments" ]
        , p [] [ text "Line comments start with ", Fixpoint.Code.inline "#", text " and run to the end of the line." ]
        , Fixpoint.Code.block
            [ text "# this is a comment\n"
            , text "tc(X, Y) :- edge(X, Y).        # transitive closure base case\n"
            , text "tc(X, Y) :- edge(X, Z), tc(Z, Y)."
            ]
        , h2 [ id "facts" ] [ text "Facts" ]
        , p []
            [ text "A fact is a ground predicate with constants only — a predicate followed by a parenthesised argument list, terminated by a full stop. There is no "
            , Fixpoint.Code.inline ":-"
            , text "."
            ]
        , Fixpoint.Code.block
            [ text "edge(1, 2).\n"
            , text "edge(2, 3).\n"
            , text "person(\"alice\").\n"
            ]
        , h2 [ id "rules" ] [ text "Rules" ]
        , p []
            [ text "A rule is "
            , Fixpoint.Code.inline "head :- body1, body2, ..., bodyN."
            , text " The body atoms are conjoined with commas; the head is a single atom. Rules may reference existing relations (arity must match) or declare a new derived relation."
            ]
        , Fixpoint.Code.block
            [ text "path(X, Y) :- edge(X, Y).\n"
            , text "tc(X, Y) :- edge(X, Y).\n"
            , text "tc(X, Y) :- edge(X, Z), tc(Z, Y).\n"
            ]
        , p []
            [ text "Multiple rules with the "
            , em [] [ text "same head predicate" ]
            , text " form a union: the engine materialises each rule and unions the results, deduplicating. There is no "
            , Fixpoint.Code.inline ";"
            , text " disjunction operator — same-head rules are the way to express a disjunction."
            ]
        , Fixpoint.Code.block
            [ text "p(X) :- edge(X, Y).\n"
            , text "p(Y) :- edge(X, Y).   # p = union of the two rules' results, deduplicated\n"
            ]
        , h2 [ id "vars" ] [ text "Variables, constants, integers and strings" ]
        , ul []
            [ li []
                [ strong [] [ text "Variables" ]
                , text " start with an uppercase letter "
                , Fixpoint.Code.inline "A-Z"
                , text " or underscore "
                , Fixpoint.Code.inline "_"
                , text ", then any alphanumeric or underscore."
                ]
            , li []
                [ strong [] [ text "Symbol constants" ]
                , text " are bare lowercase identifiers — "
                , Fixpoint.Code.inline "foo"
                , text ", "
                , Fixpoint.Code.inline "red"
                , text ". They are interned to a symbol id."
                ]
            , li []
                [ strong [] [ text "String constants" ]
                , text " are "
                , em [] [ text "double-quoted" ]
                , text ": "
                , Fixpoint.Code.inline "\"alice\""
                , text ". These are interned symbol values, used anywhere a constant appears."
                ]
            , li []
                [ strong [] [ text "Integer constants" ]
                , text " are decimal literals "
                , Fixpoint.Code.inline "0 1 2 ..."
                , text ". Raw integer literals are limited to < 2^31 (2147483648 is rejected) so they can never collide with list handles. "
                , Fixpoint.Code.inline "2147483647"
                , text " is legal."
                ]
            ]
        , Fixpoint.Code.block
            [ text "q(\"alice\").          # double-quoted string constant\n"
            , text "q(foo).              # bare lowercase symbol constant\n"
            , text "q(42).               # integer literal (raw u32)\n"
            ]
        , p []
            [ text "Note the asymmetry: "
            , Fixpoint.Code.inline "\"foo\""
            , text " (double quotes) is a string constant; "
            , Fixpoint.Code.inline "'...'"
            , text " (single quotes) is a "
            , em [] [ text "regex pattern" ]
            , text " and is only valid after "
            , Fixpoint.Code.inline "~"
            , text " (see "
            , a [ href "#regex" ] [ text "Regex" ]
            , text "). A bare lowercase identifier is itself a symbol constant."
            ]
        , h2 [ id "recursion" ] [ text "Recursion" ]
        , p []
            [ text "Recursive rules are evaluated with a semi-naive fixpoint. A predicate is recursive if it appears in its own body, directly or through a cycle."
            ]
        , Fixpoint.Code.block
            [ text "tc(X, Y) :- edge(X, Y).\n"
            , text "tc(X, Y) :- edge(X, Z), tc(Z, Y).     # recursive: tc in its own body\n"
            ]
        , p []
            [ text "Aggregates are "
            , em [] [ text "not" ]
            , text " allowed in recursive rules (a compile error)."
            ]
        , h2 [ id "negation" ] [ text "Negation" ]
        , p []
            [ text "A negated body atom is prefixed with "
            , Fixpoint.Code.inline "!"
            , text ". Negation is "
            , em [] [ text "stratified" ]
            , text ": a variable in a negated atom must be bound by a positive body atom first, and negation through recursion is rejected at compile time."
            ]
        , Fixpoint.Code.block
            [ text "path(X, Y) :- edge(X, Y), !blocked(X, Y).\n"
            , text "reachable(X) :- edge(X, Y), !blocked(X).\n"
            ]
        , p []
            [ text "Un-stratifiable programs (a strict dependency cycle through negation) are a loud compile error."
            ]
        , h2 [ id "equality" ] [ text "Equality" ]
        , p []
            [ text "Body equality is written "
            , Fixpoint.Code.inline "X = Y"
            , text " (both sides variables). It acts as a filter when both sides are bound, and binds an unbound variable from its bound counterpart otherwise."
            ]
        , Fixpoint.Code.block
            [ text "q(X, Y) :- edge(X, Y), X = Y.        # keep rows where X == Y\n"
            , text "q(Y)    :- edge(X, Y), X = Y.        # bind Y from an already-bound X\n"
            ]
        , h2 [ id "comparisons" ] [ text "Comparisons" ]
        , p []
            [ text "Ordering comparisons "
            , Fixpoint.Code.inline "<"
            , text " "
            , Fixpoint.Code.inline "<="
            , text " "
            , Fixpoint.Code.inline ">"
            , text " "
            , Fixpoint.Code.inline ">="
            , text " accept a variable or an integer constant on either side. Inequality "
            , Fixpoint.Code.inline "!="
            , text " additionally accepts a symbol constant on the right (it interns the symbol and compares symbol ids). A symbol constant is "
            , em [] [ text "not" ]
            , text " allowed in an ordering comparison."
            ]
        , Fixpoint.Code.block
            [ text "lt(X, Y) :- pair(X, Y), X < Y.\n"
            , text "le(X, Y) :- pair(X, Y), X <= Y.\n"
            , text "gt(X, Y) :- pair(X, Y), X > Y.\n"
            , text "ge(X, Y) :- pair(X, Y), X >= Y.\n"
            , text "ne(X, Y) :- pair(X, Y), X != Y.\n"
            , text "r(X)     :- val(X), X != foo.        # != accepts a symbol-constant RHS\n"
            ]
        , p []
            [ text "An ungrounded comparison operand (a variable not bound by any positive body atom) is a loud compile error."
            ]
        , h2 [ id "arithmetic" ] [ text "Arithmetic" ]
        , p []
            [ text "Arithmetic is written "
            , Fixpoint.Code.inline "X = <expr>"
            , text " in the body, producing the value "
            , Fixpoint.Code.inline "X"
            , text ". Operators are "
            , Fixpoint.Code.inline "+ - * / %"
            , text "; "
            , Fixpoint.Code.inline "* / %"
            , text " bind tighter than "
            , Fixpoint.Code.inline "+ -"
            , text ", all left-associative, with parentheses allowed. Operands are variables and integer constants (a symbol constant is rejected — symbols have no numeric value). Arithmetic wraps at u32 ("
            , Fixpoint.Code.inline "0 - 1 == 0xFFFFFFFF"
            , text "), and division/modulo by a literal "
            , Fixpoint.Code.inline "0"
            , text " is a compile error (a variable divisor of 0 simply yields no tuple)."
            ]
        , Fixpoint.Code.block
            [ text "r1(X) :- pair(A, B, C), X = A + B * C.      # precedence: A + (B*C)\n"
            , text "r2(X) :- pair(A, B, C), X = (A + B) * C.    # parentheses\n"
            , text "add(X, Y, S) :- pair(X, Y), S = X + Y.\n"
            , text "inc(X)       :- pair(A, B, C), X = A + 1.   # integer constant operand\n"
            ]
        , p []
            [ text "Arithmetic in a recursive rule works (bounded by the fixpoint), and the result variable must not be bound before the arithmetic is evaluated."
            ]
        , h2 [ id "aggregates" ] [ text "Aggregates" ]
        , p []
            [ text "Aggregates appear "
            , em [] [ text "in the body" ]
            , text ", binding a result variable: "
            , Fixpoint.Code.inline "N = count()"
            , text ", "
            , Fixpoint.Code.inline "S = sum(Y)"
            , text ", "
            , Fixpoint.Code.inline "M = min(Y)"
            , text ", "
            , Fixpoint.Code.inline "M = max(Y)"
            , text ". The result variable is referenced in the head, and every "
            , em [] [ text "other" ]
            , text " head variable is the implicit group-by key."
            ]
        , Fixpoint.Code.block
            [ text "# group by X: number of outgoing edges per node\n"
            , text "cnt(X, N) :- edge(X, Y), N = count().\n"
            , text "\n"
            , text "# group by X: total of Y per X\n"
            , text "total(X, S) :- edge(X, Y), S = sum(Y).\n"
            , text "\n"
            , text "# min / max of Y per X\n"
            , text "minv(X, M) :- edge(X, Y), M = min(Y).\n"
            , text "maxv(X, M) :- edge(X, Y), M = max(Y).\n"
            , text "\n"
            , text "# no group-by vars -> a single global count\n"
            , text "cnt(N) :- edge(X, Y), N = count().\n"
            ]
        , p [] [ text "Constraints on aggregates:" ]
        , ul []
            [ li [] [ Fixpoint.Code.inline "count()", text " takes no arguments; ", Fixpoint.Code.inline "sum/min/max", text " take exactly one variable argument." ]
            , li [] [ text "Only one aggregate is allowed per rule." ]
            , li [] [ text "Aggregates are not allowed inside negation, in recursive rules, or in rules whose head contains a constant." ]
            , li [] [ text "Aggregates over a variadic relation are rejected." ]
            ]
        , h2 [ id "strings" ] [ text "String builtins" ]
        , p [] [ text "String values are interned symbols. Two kinds of builtin exist:" ]
        , ul []
            [ li []
                [ strong [] [ text "Producers" ]
                , text " (bind a result variable): "
                , Fixpoint.Code.inline "C = concat(A, B)"
                , text ", "
                , Fixpoint.Code.inline "N = length(S)"
                , text ", "
                , Fixpoint.Code.inline "L = lower(S)"
                , text ", "
                , Fixpoint.Code.inline "U = upper(S)"
                , text "."
                ]
            , li []
                [ strong [] [ text "Filters" ]
                , text " (test a string): "
                , Fixpoint.Code.inline "prefix(S, P)"
                , text ", "
                , Fixpoint.Code.inline "suffix(S, P)"
                , text ", "
                , Fixpoint.Code.inline "contains(S, P)"
                , text "."
                ]
            ]
        , Fixpoint.Code.block
            [ text "cat(A, B, C) :- pair(A, B), C = concat(A, B).\n"
            , text "lens(A, N)   :- str(A),    N = length(A).       # byte length\n"
            , text "lc(X)        :- s(Y),      X = lower(Y).        # ASCII case folding\n"
            , text "uc(X)        :- s(Y),      X = upper(Y).\n"
            , text "pref(X)      :- str(X),    prefix(X, \"he\").\n"
            , text "suf(X)       :- str(X),    suffix(X, \"o\").\n"
            , text "cont(X)      :- str(X),    contains(X, \"ll\").\n"
            ]
        , p []
            [ text ""
            , Fixpoint.Code.inline "lower"
            , text " and "
            , Fixpoint.Code.inline "upper"
            , text " are implemented (ASCII A–Z folding). "
            , Fixpoint.Code.inline "length"
            , text " is the byte length (UTF-8 bytes, not codepoints), and it also accepts a constant list literal (list length). String operands must be variables or double-quoted string constants — a raw integer operand is rejected. A producer result longer than 4096 bytes backtracks (no tuple)."
            ]
        , h2 [ id "lists" ] [ text "Lists" ]
        , p []
            [ text "Lists are first-class values, interned in a term store (equal lists share one handle). List literals are written "
            , Fixpoint.Code.inline "[e1, e2, ...]"
            , text "; "
            , Fixpoint.Code.inline "[]"
            , text " is the empty list."
            ]
        , ul []
            [ li []
                [ strong [] [ text "Producers" ]
                , text ": "
                , Fixpoint.Code.inline "L = cons(H, T)"
                , text ", "
                , Fixpoint.Code.inline "H = car(L)"
                , text ", "
                , Fixpoint.Code.inline "T = cdr(L)"
                , text ", "
                , Fixpoint.Code.inline "L = append(A, B)"
                , text "."
                ]
            , li []
                [ strong [] [ text "Filter / generator" ]
                , text ": "
                , Fixpoint.Code.inline "member(X, L)"
                , text " — tests X when X is bound, enumerates L’s elements when X is unbound."
                ]
            , li []
                [ strong [] [ text "Patterns" ]
                , text ": "
                , Fixpoint.Code.inline "[X | Xs]"
                , text " destructures a list into head and tail; it can appear as a relational argument (e.g. "
                , Fixpoint.Code.inline "p([H | T])"
                , text ") or as a list-assignment form "
                , Fixpoint.Code.inline "[H | T] = L"
                , text "."
                ]
            ]
        , Fixpoint.Code.block
            [ text "r(X, H, T, A) :- p(X), L = cons(X, [7, 8]), H = car(L),\n"
            , text "                 T = cdr(L), A = append(L, [9]).\n"
            , text "q(H, T)       :- p([H | T]).                 # pattern in a relational arg\n"
            , text "r(X)          :- p([7, X]).                  # constant element + var\n"
            , text "r(X)          :- p(L), member(X, L).         # generator\n"
            , text "s(X)          :- num(X), p(L), member(X, L). # filter\n"
            , text "empty_tail(A, B) :- [A, B] = [1, 2].         # assignment form, empty tail\n"
            , text "tail_pat(H, T)   :- [H | T] = [1, 2, 3].     # assignment form, tail\n"
            , text "from_var(H, T)   :- p(L), [H | T] = L.       # assignment from a var\n"
            ]
        , p []
            [ text "A list pattern is not allowed in a rule head or fact (use "
            , Fixpoint.Code.inline "cons"
            , text " to build), or in a negated atom (patterns cannot bind variables under negation). The tail after "
            , Fixpoint.Code.inline "|"
            , text " must be a variable."
            ]
        , h2 [ id "range" ] [ text "Range" ]
        , p []
            [ text ""
            , Fixpoint.Code.inline "range(X, Rel, Lo, Hi)"
            , text " is a reserved builtin that scans the "
            , em [] [ text "distinct leading-column values" ]
            , text " of the relation "
            , Fixpoint.Code.inline "Rel"
            , text " in the half-open interval "
            , Fixpoint.Code.inline "[Lo, Hi)"
            , text ":"
            ]
        , ul []
            [ li [] [ Fixpoint.Code.inline "X", text " is the variable, ", strong [] [ text "first" ], text "." ]
            , li [] [ Fixpoint.Code.inline "Rel", text " is the relation ", em [] [ text "name" ], text " (an identifier, not a variable)." ]
            , li [] [ Fixpoint.Code.inline "Lo", text " and ", Fixpoint.Code.inline "Hi", text " are the half-open bounds (variable or integer constant)." ]
            ]
        , p []
            [ text "When "
            , Fixpoint.Code.inline "X"
            , text " is unbound, "
            , Fixpoint.Code.inline "range"
            , text " acts as a generator yielding each distinct "
            , Fixpoint.Code.inline "col0"
            , text " value of "
            , Fixpoint.Code.inline "Rel"
            , text " in "
            , Fixpoint.Code.inline "[Lo, Hi)"
            , text ", in lex order. When "
            , Fixpoint.Code.inline "X"
            , text " is bound it acts as a filter. The range is over the leading column only."
            ]
        , Fixpoint.Code.block
            [ text "q(X) :- range(X, r, 10, 20).          # X in {col0 of r in [10, 20)}\n"
            , text "p(X) :- r(X), range(X, r, 10, 20).    # filter form\n"
            , text "q(X, Lo, Hi) :- bounds(Lo, Hi), range(X, r, Lo, Hi).   # var bounds\n"
            ]
        , p []
            [ Fixpoint.Code.inline "Rel"
            , text " must be a known, non-variadic relation of arity ≥ 1. Range over a recursive relation, an unknown relation, a negated range, or an ungrounded bound variable is a loud compile error."
            ]
        , h2 [ id "regex" ] [ text "Regex patterns" ]
        , p []
            [ text "A body atom may carry a regex pattern written as a "
            , em [] [ text "single-quoted" ]
            , text " string after a tilde: "
            , Fixpoint.Code.inline "pred(...) ~ 'pattern'"
            , text " or, to target a specific column, "
            , Fixpoint.Code.inline "pred(...) ~ k 'pattern'"
            , text " where "
            , Fixpoint.Code.inline "k"
            , text " is a 0-based integer column index (default 0 = leading column). The pattern is compiled to a DFA and matched against the "
            , em [] [ text "string content" ]
            , text " of that column’s value (via the symbols table), not the raw binary key — so "
            , Fixpoint.Code.inline "~ 'a.*'"
            , text " means “column’s text starts with "
            , Fixpoint.Code.inline "a"
            , text "”. An integer (non-string) column matches nothing. In the CLI, use "
            , Fixpoint.Code.inline "pattern <rel> [<col>] '<pattern>'"
            , text " for standalone regex queries."
            ]
        , Fixpoint.Code.block
            [ text "q(X, Y) :- edge(X, Y) ~ 1 '(a|b).*'.   # filter on column 1's text\n"
            ]
        , p []
            [ text "A negated pattern atom is not supported (compile error). A column index out of range for the atom is also a compile error. In rules, patterns are compiled at load time; a bad regex is a loud error."
            ]
        , h2 [ id "variadic" ] [ text "Variadic relations" ]
        , p []
            [ text "A relation may be declared "
            , em [] [ text "variadic" ]
            , text ", accepting facts of any arity 1–8. Storage is per-arity fixed-width; rule atoms resolve to the variant matching their syntactic argument count. A variadic head must be declared before "
            , Fixpoint.Code.inline "dl_load_rules"
            , text ". Aggregates over a variadic relation, recursive variadic heads, and magic/top-down queries over programs containing variadics are rejected at compile time (they always evaluate via the full fixpoint)."
            ]
        , Fixpoint.Code.block
            [ text "/* C API: */\n"
            , text "dl_declare_relation_variadic(db, \"v\");\n"
            ]
        , h2 [ id "reserved" ] [ text "Reserved names" ]
        , p []
            [ text "The builtin names are reserved — a rule head cannot use them (rejected with a clear diagnostic): "
            , Fixpoint.Code.inline "member"
            , text ", "
            , Fixpoint.Code.inline "car"
            , text ", "
            , Fixpoint.Code.inline "cons"
            , text ", "
            , Fixpoint.Code.inline "cdr"
            , text ", "
            , Fixpoint.Code.inline "append"
            , text ", "
            , Fixpoint.Code.inline "concat"
            , text ", "
            , Fixpoint.Code.inline "length"
            , text ", "
            , Fixpoint.Code.inline "lower"
            , text ", "
            , Fixpoint.Code.inline "upper"
            , text ", "
            , Fixpoint.Code.inline "prefix"
            , text ", "
            , Fixpoint.Code.inline "suffix"
            , text ", "
            , Fixpoint.Code.inline "contains"
            , text ", "
            , Fixpoint.Code.inline "range"
            , text "."
            ]
        , h2 [ id "errors" ] [ text "Compile-error semantics" ]
        , p []
            [ text "The engine never silently mis-evaluates. Unsupported or malformed programs are rejected with a diagnostic at "
            , em [] [ text "compile time" ]
            , text " and "
            , Fixpoint.Code.inline "dl_load_rules"
            , text " / "
            , Fixpoint.Code.inline "dl_compile"
            , text " return -1. Rejected cases include:"
            ]
        , ul []
            [ li [] [ text "unstratifiable negation or an unstratifiable strict cycle through range;" ]
            , li [] [ text "unsafe negation (a variable not bound by a positive atom);" ]
            , li [] [ text "ungrounded comparison, arithmetic, string, list, member, or range operands;" ]
            , li [] [ text "division/modulo by a literal ", Fixpoint.Code.inline "0", text ";" ]
            , li [] [ text "aggregates in recursive rules, two aggregates, negated aggregates, constants in an aggregate head;" ]
            , li [] [ text "list patterns in heads/facts or negated atoms; a reserved name as a head;" ]
            , li [] [ text "range over a recursive / unknown / variadic relation;" ]
            , li [] [ text "integer literals ≥ 2^31; a symbol constant in an ordering comparison or arithmetic;" ]
            , li [] [ text "a rule with no positive body atom (unless a lone ", Fixpoint.Code.inline "member", text "/", Fixpoint.Code.inline "range", text "/list-assignment drives it)." ]
            ]
        ]


cliView : Html Msg
cliView =
    div []
        [ h1 [] [ text "CLI Reference" ]
        , p []
            [ text "The "
            , Fixpoint.Code.inline "dl"
            , text " binary is the command-line front end. It has commands for loading facts, querying relations, publishing snapshots, and the search tier (full-text + semantic). The database directory defaults to "
            , Fixpoint.Code.inline "dl-test-db"
            , text " and can be set with "
            , Fixpoint.Code.inline "-d <dir>"
            , text ". Command values that parse as bare integers are stored raw as u32; anything else is interned to a string symbol id."
            ]
        , Fixpoint.Code.block
            [ text "dl [-d <dir>] load    <csv> --rel <name>\n"
            , text "dl [-d <dir>] lookup  <rel> <val> [<val> ...]\n"
            , text "dl [-d <dir>] prefix  <rel> [<val> ...]\n"
            , text "dl [-d <dir>] query   '<rule>' | <file.dl> <goal-rel>\n"
            , text "dl [-d <dir>] qmagic  '<rule>' | <file.dl> <goal-rel> [-a <adorn>] <val> [<val> ...]\n"
            , text "dl [-d <dir>] publish\n"
            , text "dl [-d <dir>] bound   <rel> <val> [<val> ...]\n"
            , text "dl [-d <dir>] pattern <rel> '<regex>'\n"
            , text "dl [-d <dir>] search  '<terms>' [--top N] [--version N]\n"
            , text "dl [-d <dir>] vsearch '<query>' [--k N] [--radius R] [--version V] [--sig <hex>] [--ivec <hex>]\n"
            , text "dl [-d <dir>] vhybrid '<terms>' '<query>' [--k N] [--radius R] [--version V]\n"
            , text "dl [-d <dir>] versions"
            ]
        , h2 [ id "load" ] [ text "load" ]
        , p [] [ text "Load facts from a headerless CSV file into a relation (arity 1–8)." ]
        , Fixpoint.Code.block
            [ text "$ ./dl -d /tmp/db load edges.csv --rel edge\n"
            , text "Loaded 5 facts into edge"
            ]
        , p []
            [ text "The relation’s arity is inferred from the first non-empty CSV row. Values: quoted strings are interned; bare integers are stored raw as u32."
            ]
        , h2 [ id "lookup" ] [ text "lookup" ]
        , p [] [ text "Exact lookup of a fact by its full column values. Prints ", Fixpoint.Code.inline "found", text " or ", Fixpoint.Code.inline "not found", text "." ]
        , Fixpoint.Code.block
            [ text "$ ./dl -d /tmp/db lookup edge 1 2\n"
            , text "found\n"
            , text "\n"
            , text "$ ./dl -d /tmp/db lookup edge 9 9\n"
            , text "not found"
            ]
        , h2 [ id "prefix" ] [ text "prefix" ]
        , p [] [ text "Bind the leading columns to the given values and enumerate every matching complete tuple." ]
        , Fixpoint.Code.block
            [ text "# all tuples\n"
            , text "$ ./dl -d /tmp/db prefix edge\n"
            , text "1 2\n"
            , text "1 3\n"
            , text "2 3\n"
            , text "2 4\n"
            , text "3 5\n"
            , text "\n"
            , text "# tuples with leading column == 2\n"
            , text "$ ./dl -d /tmp/db prefix edge 2\n"
            , text "2 3\n"
            , text "2 4"
            ]
        , p [] [ text "With no bound values, this lists the whole relation." ]
        , h2 [ id "query" ] [ text "query" ]
        , p []
            [ text "Parse, compile, and run a Datalog rule in one step, then stream the goal relation’s tuples. The rule source may be a quoted inline string or a "
            , Fixpoint.Code.inline ".dl"
            , text " file path; the goal is the relation to print."
            ]
        , Fixpoint.Code.block
            [ text "$ ./dl -d /tmp/db query \\\n"
            , text "    'tc(X,Y) :- edge(X,Y). tc(X,Y) :- edge(X,Z), tc(Z,Y).' tc\n"
            , text "1 2\n"
            , text "1 3\n"
            , text "1 4\n"
            , text "1 5\n"
            , text "2 3\n"
            , text "2 4\n"
            , text "2 5\n"
            , text "3 5"
            ]
        , p []
            [ text "Internally this loads the rules, publishes a snapshot (running the VM if the fixpoint is dirty), then queries the goal relation."
            ]
        , h2 [ id "qmagic" ] [ text "qmagic" ]
        , p []
            [ text "Magic-sets bound query: evaluates a "
            , em [] [ text "scoped" ]
            , text " fixpoint seeded by the bound values, materialising only the reachable IDB slice. The result is byte-for-byte identical to "
            , Fixpoint.Code.inline "dl_query_bound"
            , text " over the fully materialized goal."
            ]
        , Fixpoint.Code.block
            [ text "# leading-prefix form: bind the first k args\n"
            , text "$ ./dl -d /tmp/db qmagic \\\n"
            , text "    'tc(X,Y) :- edge(X,Y). tc(X,Y) :- edge(X,Z), tc(Z,Y).' tc 1\n"
            , text "\n"
            , text "# arbitrary-adornment form: -a <adorn> binds named positions\n"
            , text "$ ./dl -d /tmp/db qmagic \\\n"
            , text "    'tc(X,Y) :- edge(X,Y). tc(X,Y) :- edge(X,Z), tc(Z,Y).' tc -a \"bf\" 1"
            ]
        , p []
            [ text "The optional "
            , Fixpoint.Code.inline "-a <adorn>"
            , text " is a string of exactly "
            , Fixpoint.Code.inline "goal-arity"
            , text " characters, each "
            , Fixpoint.Code.inline "b"
            , text " (bound) or "
            , Fixpoint.Code.inline "f"
            , text " (free); "
            , Fixpoint.Code.inline "vals"
            , text " are packed left-to-right in the order of the "
            , Fixpoint.Code.inline "b"
            , text " positions. Programs using negation, aggregates, or cross-predicate mutual recursion are rejected with a diagnostic."
            ]
        , h2 [ id "time" ] [ text "Time travel" ]
        , p []
            [ text "The CLI exposes the snapshot timeline directly. "
            , Fixpoint.Code.inline "publish"
            , text " writes an immutable, versioned snapshot; "
            , Fixpoint.Code.inline "versions"
            , text " lists the history; "
            , Fixpoint.Code.inline "bound"
            , text " reads the current snapshot view; and the search tier accepts "
            , Fixpoint.Code.inline "--version N"
            , text " to query "
            , em [] [ text "as-of" ]
            , text " a past snapshot. Together these make time-traveling queries a first-class CLI capability."
            ]
        , Fixpoint.Code.block
            [ Fixpoint.Code.c "# Record a point-in-time, then inspect the history."
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " "
            , Fixpoint.Code.g "./dl -d /tmp/db publish"
            , text "\n"
            , Fixpoint.Code.g "Snapshot published."
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " "
            , Fixpoint.Code.g "./dl -d /tmp/db versions"
            , text "\n"
            , Fixpoint.Code.g "1\n2\n3"
            , text "\n\n"
            , Fixpoint.Code.c "# Search as-of a past snapshot (full-text + semantic)."
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " "
            , Fixpoint.Code.g "./dl -d /tmp/db search 'gpu rental' --top 10 --version 2"
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " "
            , Fixpoint.Code.g "./dl -d /tmp/db vsearch 'GPU rental' --k 10 --version 2"
            ]
        , p [] [ text "See ", a [ href "https://fixpointlinux.org/datalog-dafsa/time-travel/", attribute "data-mfe-route" "/datalog-dafsa/time-travel" ] [ text "the time-travel guide" ], text " for the full as-of API." ]
        , h2 [ id "publish" ] [ text "publish" ]
        , p [] [ text "Atomically publish a versioned snapshot of the database — the write side of time travel." ]
        , Fixpoint.Code.block
            [ text "$ ./dl -d /tmp/db publish\n"
            , text "Snapshot published."
            ]
        , p [] [ text "After publishing, ", Fixpoint.Code.inline "query", text " reads from mmap instead of running the VM." ]
        , h2 [ id "bound" ] [ text "bound" ]
        , p [] [ text "Bound query (snapshot path): bind leading columns and enumerate via the snapshot view." ]
        , Fixpoint.Code.block
            [ text "$ ./dl -d /tmp/db bound edge 1\n"
            , text "1 2\n"
            , text "1 3"
            ]
        , h2 [ id "pattern" ] [ text "pattern" ]
        , p [] [ text "Enumerate all tuples whose full key matches a regex." ]
        , Fixpoint.Code.block
            [ text "$ ./dl -d /tmp/db pattern edge '(a|b).*'"
            ]
        , p [] [ text "A bad pattern is a loud error." ]
        , h2 [ id "search" ] [ text "search" ]
        , p []
            [ text "Full-text search over the "
            , Fixpoint.Code.inline "__postings__"
            , text " index: AND-intersect the tokenized terms and rank by co-occurrence. "
            , Fixpoint.Code.inline "--version N"
            , text " queries the index as-of a published snapshot (0 = live)."
            ]
        , Fixpoint.Code.block
            [ text "$ ./dl -d /tmp/db search 'gpu rental' --top 10\n"
            , text "$ ./dl -d /tmp/db search 'gpu rental' --top 10 --version 3"
            ]
        , h2 [ id "vsearch" ] [ text "vsearch" ]
        , p []
            [ text "Semantic vector search: embed the query with the bge-small model (via the "
            , Fixpoint.Code.inline "dl-embed"
            , text " tool), retrieve MIH candidates from the "
            , Fixpoint.Code.inline "__sig*__"
            , text " postings, and re-rank by in-store int8 cosine. Accepts "
            , Fixpoint.Code.inline "--sig"
            , text "/"
            , Fixpoint.Code.inline "--ivec"
            , text " hex for a programmatic path that needs no model, and "
            , Fixpoint.Code.inline "--version V"
            , text " to search as-of a snapshot."
            ]
        , Fixpoint.Code.block
            [ text "$ ./dl -d /tmp/db vsearch 'affordable GPU rental' --k 10"
            ]
        , p [] [ text "See ", a [ href "https://fixpointlinux.org/datalog-dafsa/vector-search/", attribute "data-mfe-route" "/datalog-dafsa/vector-search" ] [ text "the vector-search page" ], text " for the full semantic tier." ]
        , h2 [ id "vhybrid" ] [ text "vhybrid" ]
        , p [] [ text "Lexical ∩ semantic hybrid: intersect ", Fixpoint.Code.inline "search", text " results with ", Fixpoint.Code.inline "vsearch", text " candidates, then re-rank the intersection." ]
        , Fixpoint.Code.block
            [ text "$ ./dl -d /tmp/db vhybrid 'gpu rental' 'affordable GPU rental' --k 10"
            ]
        , h2 [ id "versions" ] [ text "versions" ]
        , p [] [ text "List the published snapshot versions (ascending) — the timeline." ]
        , Fixpoint.Code.block
            [ text "$ ./dl -d /tmp/db versions"
            ]
        , h2 [ id "scope" ] [ text "What is ", em [] [ text "not" ], text " in the CLI" ]
        , p []
            [ text "The top-down / QSQ path ("
            , Fixpoint.Code.inline "dl_query_topdown"
            , text " / "
            , Fixpoint.Code.inline "dl_query_topdown_adorn"
            , text ") is available only through the C API — there is "
            , strong [] [ text "no" ]
            , text " "
            , Fixpoint.Code.inline "topdown"
            , text " CLI subcommand. Likewise, order-statistics (rank / select / range / count) are C-API only. The semantic search tier relies on the "
            , Fixpoint.Code.inline "dl-embed"
            , text " companion tool (built with "
            , Fixpoint.Code.inline "make dl-embed"
            , text ") for query-time embedding."
            ]
        ]


apiView : Html Msg
apiView =
    div []
        [ h1 [] [ text "C API Reference" ]
        , p []
            [ text "The public C API is declared in "
            , Fixpoint.Code.inline "src/dl.h"
            , text ". All value arrays are u32 (raw integers or interned symbol ids). The handle type is the opaque "
            , Fixpoint.Code.inline "dl_db"
            , text ". This page documents the complete surface, grouped by area. See also "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/order-statistics/", attribute "data-mfe-route" "/datalog-dafsa/order-statistics" ] [ text "Order Statistics" ]
            , text ", "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/time-travel/", attribute "data-mfe-route" "/datalog-dafsa/time-travel" ] [ text "Time Travel" ]
            , text ", and "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/vector-search/", attribute "data-mfe-route" "/datalog-dafsa/vector-search" ] [ text "Vector Search" ]
            , text " for the larger feature groups."
            ]
        , h2 [ id "lifecycle" ] [ text "Lifecycle" ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "dl_db *dl_open(const char *dir)" ], td [] [ text "Open or create a database directory. Returns NULL on error." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "void dl_close(dl_db *db)" ], td [] [ text "Close the database, flushing and saving state." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "dl_db *dl_open2(const char *dir, int *err_out)" ], td [] [ text "Open with explicit error reporting. On failure sets ", Fixpoint.Code.inline "*err_out", text " (see ", Fixpoint.Code.inline "DL_E_LOCKED", text ") and returns NULL." ] ]
                ]
            ]
        , h2 [ id "schema" ] [ text "Schema" ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "int dl_declare_relation(dl_db *db, const char *name, uint8_t arity)" ], td [] [ text "Declare a fixed-arity relation (1–8). Idempotent; arity 0 declares a variadic relation." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "int dl_declare_relation_variadic(dl_db *db, const char *name)" ], td [] [ text "Declare a variadic relation: facts of any arity 1–8 are accepted, stored per-arity." ] ]
                ]
            ]
        , h2 [ id "facts" ] [ text "Facts" ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "int dl_load_facts(dl_db *db, const char *rel, const char *csv_path)" ], td [] [ text "Load ground facts from a headerless CSV (quoted strings interned; bare integers raw u32). Returns facts loaded, or -1." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "int dl_add_fact(dl_db *db, const char *rel, const uint32_t *cols, uint8_t arity)" ], td [] [ text "Add a single fact (durable WAL + fsync). Returns 1 added / 0 duplicate / -1 error." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "int dl_delete_fact(dl_db *db, const char *rel, const uint32_t *cols, uint8_t arity)" ], td [] [ text "Delete a single fact. Returns 1 deleted / 0 absent / -1 error." ] ]
                ]
            ]
        , h2 [ id "query" ] [ text "Queries" ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "int dl_lookup(dl_db *db, const char *rel, const uint32_t *cols, uint8_t arity)" ], td [] [ text "Exact lookup. Returns 1 if present, else 0." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "typedef int (*dl_tuple_cb)(const uint32_t *cols, uint8_t arity, void *user)" ], td [] [ text "Tuple enumeration callback; return non-zero to stop early." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_prefix(dl_db *db, const char *rel, const uint32_t *leading, uint8_t k, dl_tuple_cb cb, void *user)" ], td [] [ text "Bind the first k columns and enumerate matching tuples. Returns count, or -1." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_query(dl_db *db, const char *goal_rel, dl_tuple_cb cb, void *user)" ], td [] [ text "Stream the goal relation’s tuples. Reads mmap snapshot if published, else runs the VM. Returns tuple count, or -1." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_query_bound(dl_db *db, const char *goal_rel, const uint32_t *leading, uint8_t k, dl_tuple_cb cb, void *user)" ], td [] [ text "Prefix-bind k columns of the goal and enumerate." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_pattern(dl_db *db, const char *rel, const struct regex_dfa *dfa, dl_tuple_cb cb, void *user)" ], td [] [ text "Enumerate tuples whose full key matches a compiled regex DFA." ] ]
                ]
            ]
        , h2 [ id "iter" ] [ text "Iterator + merge-join" ]
        , p []
            [ text "A resumable pull-based cursor over the tuples of a relation in ascending key order (u32BE key encoding ⇒ numeric order == lex order). Reads from the mmap snapshot view when a snapshot is current, else the in-memory relation."
            ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "dl_iter *dl_iter_open(dl_db *db, const char *rel, const uint32_t *leading, uint8_t k)" ], td [] [ text "Open a cursor bound to the first k leading columns (k==0: all). Returns NULL on error; an absent prefix yields a valid empty iterator." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "int dl_iter_seek(dl_iter *it, const uint32_t *leading, uint8_t k)" ], td [] [ text "Re-bind the cursor to a new leading prefix. Returns 0 / -1." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "int dl_iter_next(dl_iter *it, uint32_t *cols_out)" ], td [] [ text "Fetch the next tuple ascending. Returns 1 / 0 at end / -1 error." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "uint8_t dl_iter_arity(const dl_iter *it)" ], td [] [ text "The relation’s arity (0 for a NULL/error cursor)." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "void dl_iter_close(dl_iter *it)" ], td [] [ text "Close the cursor (NULL-safe)." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "typedef int (*dl_join_cb)(const uint32_t *l, uint8_t la, const uint32_t *r, uint8_t ra, void *user)" ], td [] [ text "Merge-join pair callback." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_merge_join(dl_iter *l, dl_iter *r, uint8_t jcols, dl_join_cb cb, void *user)" ], td [] [ text "Equi-join two sorted iterators on their first jcols columns, streaming pairs in sorted order (cross-product semantics, duplicates preserved). Both iterators left exhausted. Returns pairs emitted, or -1." ] ]
                ]
            ]
        , h2 [ id "order-stats" ] [ text "Order statistics" ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "uint64_t dl_rank(dl_db *db, const char *rel, const uint32_t *cols, uint8_t arity)" ], td [] [ text "Number of distinct tuples strictly lexicographically smaller than ", Fixpoint.Code.inline "cols", text ". ", Fixpoint.Code.inline "UINT64_MAX", text " on error." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "int dl_select(dl_db *db, const char *rel, uint64_t k, uint32_t *cols_out, uint8_t arity)" ], td [] [ text "The k-th tuple (0-indexed, lex order). Returns 0 / -1." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "uint64_t dl_range_count(dl_db *db, const char *rel, const uint32_t *lo, const uint32_t *hi, uint8_t arity)" ], td [] [ text "Number of distinct tuples in the half-open range [lo, hi) = rank(hi) − rank(lo)." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "uint64_t dl_count(dl_db *db, const char *rel)" ], td [] [ text "O(1) distinct-tuple count (memoized subtree array)." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "uint64_t dl_rank_bound(db, rel, leading, k, cols, arity)" ], td [] [ text "Rank restricted to tuples whose first k columns equal ", Fixpoint.Code.inline "leading", text "." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "int dl_select_bound(db, rel, leading, k, idx, cols_out, arity)" ], td [] [ text "Select within a leading-prefix bound." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "uint64_t dl_range_count_bound(db, rel, leading, k, lo, hi, arity)" ], td [] [ text "Range count within a leading-prefix bound." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "uint64_t dl_rank_perm(db, rel, perm_id, cols, arity)" ], td [] [ text "Rank over a permuted view (order-by on a non-leading column)." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "int dl_select_perm(db, rel, perm_id, k, cols_out, arity)" ], td [] [ text "Select over a permuted view (inverse-maps the result back to original column order)." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "uint64_t dl_range_count_perm(db, rel, perm_id, lo, hi, arity)" ], td [] [ text "Range count over a permuted view." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "int dl_db_perm_count(const dl_db *db)" ], td [] [ text "Number of permutation indices currently declared." ] ]
                ]
            ]
        , p []
            [ text "Details and examples are on the "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/order-statistics/", attribute "data-mfe-route" "/datalog-dafsa/order-statistics" ] [ text "Order Statistics" ]
            , text " page."
            ]
        , h2 [ id "rules" ] [ text "Rules" ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "int dl_load_rules(dl_db *db, const char *dl_source)" ], td [] [ text "Parse and compile Datalog rules from a source string. Returns 0 / -1. Rules may reference existing relations or declare new derived relations." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "int dl_compile(dl_db *db)" ], td [] [ text "Compile all loaded rules into bytecode and run them (semi-naive fixpoint), materializing derived relations. Returns 0 / -1." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_query_magic(db, goal_rel, leading, k, cb, user)" ], td [] [ text "Magic-sets bound query (leading/k). Re-evaluates a scoped fixpoint; result is byte-identical to ", Fixpoint.Code.inline "dl_query_bound", text ". Returns count, or -1." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_query_magic_adorn(db, goal_rel, adorn, vals, nvals, cb, user)" ], td [] [ text "Magic-sets with an arbitrary adornment string of ", Fixpoint.Code.inline "b", text "/", Fixpoint.Code.inline "f", text " chars." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_query_topdown(db, goal_rel, leading, k, cb, user)" ], td [] [ text "Top-down / QSQ: the same adorned magic program, scheduled demand-driven (SLG worklist) instead of forward semi-naive. C-API only — no CLI command." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_query_topdown_adorn(db, goal_rel, adorn, vals, nvals, cb, user)" ], td [] [ text "Top-down / QSQ with an arbitrary adornment." ] ]
                ]
            ]
        , h2 [ id "snapshot" ] [ text "Snapshot" ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "int dl_publish_snapshot(dl_db *db)" ], td [] [ text "Atomically save the interner + all relations to a versioned snapshot directory and flip the CURRENT pointer. After publish, ", Fixpoint.Code.inline "dl_query", text " reads from mmap." ] ]
                ]
            ]
        , h2 [ id "time-travel" ] [ text "Time-travel / as-of" ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "long dl_snapshot_versions(const dl_db *db, uint32_t *out, size_t cap)" ], td [] [ text "Enumerate every published snapshot version ascending. Returns the total (two-call idiom)." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_query_version(dl_db *db, uint32_t version, const char *goal_rel, dl_tuple_cb cb, void *user)" ], td [] [ text "As-of query: stream tuples as of snapshot ", Fixpoint.Code.inline "version", text ", bypassing live routing. Nonexistent version / absent relation is a loud -1, never a silent empty result." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_query_bound_version(dl_db *db, uint32_t version, const char *goal_rel, const uint32_t *leading, uint8_t k, dl_tuple_cb cb, void *user)" ], td [] [ text "As-of prefix query." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "int dl_set_snapshot_retain(dl_db *db, unsigned n)" ], td [] [ text "Opt-in retention: keep at most n most-recent versions, pruning older ones after each publish. n==0 (default) keeps every version." ] ]
                ]
            ]
        , p []
            [ text "Details and examples are on the "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/time-travel/", attribute "data-mfe-route" "/datalog-dafsa/time-travel" ] [ text "Time Travel" ]
            , text " page."
            ]
        , h2 [ id "search" ] [ text "Search & vector search" ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "long dl_search(dl_db *db, const uint32_t *terms, int n_terms, dl_search_cb cb, void *user)" ], td [] [ text "Full-text search: AND-intersect tokenized terms over the postings index, rank by co-occurrence." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_search_version(dl_db *db, uint32_t version, const uint32_t *terms, int n_terms, dl_search_cb cb, void *user)" ], td [] [ text "Full-text search as-of a snapshot version." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_vector_search(dl_db *db, const uint32_t *q_sig, int k, int r, dl_vec_cb cb, void *user)" ], td [] [ text "Semantic vector search (LIVE): MIH candidate retrieval over ", Fixpoint.Code.inline "__sig{j}__", text " postings for a pre-encoded 8-word ITQ signature, then live-entity filter. Emits band-match counts via ", Fixpoint.Code.inline "dl_vec_cb", text "." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_vector_search_version(dl_db *db, uint32_t version, const uint32_t *q_sig, int k, int r, dl_vec_cb cb, void *user)" ], td [] [ text "Same, as-of a snapshot version (reads ", Fixpoint.Code.inline "__sig{j}__", text " + ", Fixpoint.Code.inline "entity", text " via ", Fixpoint.Code.inline "dl_query_bound_version", text ")." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "long dl_vector_rerank(dl_db *db, const uint32_t *q_int8, const uint32_t *cand_syms, int n_cand, int k, dl_vec_cb cb, void *user)" ], td [] [ text "Exact int8-cosine re-rank of a candidate set over ", Fixpoint.Code.inline "__vec_q__", text ", by signed int64 cross-multiply (no float). Emits exact-cosine order via ", Fixpoint.Code.inline "dl_vec_cb", text "." ] ]
                ]
            ]
        , p []
            [ text "The semantic tier is documented on the "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/vector-search/", attribute "data-mfe-route" "/datalog-dafsa/vector-search" ] [ text "Vector Search" ]
            , text " page; embedding is done by the "
            , Fixpoint.Code.inline "dl-embed"
            , text " tool (bge-small via ggml), not the C API."
            ]
        , h2 [ id "hooks" ] [ text "Fault-injection hooks (test-only)" ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "void dl_set_fault_hook(dl_db *db, int (*hook)(dl_fpoint fp, void *user), void *user)" ], td [] [ text "Install a fault-injection hook called at each fpoint during publish (", Fixpoint.Code.inline "DL_FPOINT_AFTER_REL_SAVE", text ", ", Fixpoint.Code.inline "DL_FPOINT_AFTER_RENAME", text ") and during a transaction commit (", Fixpoint.Code.inline "DL_FPOINT_TXN_BEFORE_MARKER", text "); non-zero aborts the publish/commit." ] ]
                ]
            ]
        , h2 [ id "interner" ] [ text "Interner" ]
        , table [ class "api" ]
            [ tbody []
                [ tr [] [ td [] [ Fixpoint.Code.inline "uint32_t dl_intern_str(dl_db *db, const char *str)" ], td [] [ text "Intern a string, return its sym_id (1-based). Returns 0 on OOM." ] ]
                , tr [] [ td [] [ Fixpoint.Code.inline "const char *dl_intern_str_of(dl_db *db, uint32_t sym_id)" ], td [] [ text "Look up a sym_id → string. Returns NULL if out of range." ] ]
                ]
            ]
        ]


architectureView : Html Msg
architectureView =
    div []
        [ h1 [] [ text "Architecture" ]
        , p []
            [ text "This page explains the storage thesis and the join / evaluation strategies. It is a qualitative description; see "
            , Fixpoint.Code.inline "tests/bench.c"
            , text " for the demonstration benchmark harness rather than this site for numbers."
            ]
        , h2 [ id "storage" ] [ text "The storage thesis: fixed-width big-endian keys" ]
        , p []
            [ text "The linchpin of the engine is that every fact of arity "
            , Fixpoint.Code.inline "a"
            , text " is encoded as one fixed-width key of "
            , Fixpoint.Code.inline "4*a"
            , text " bytes — each column a u32 in big-endian byte order, laid out in column order with no inter-column separator, plus a trailing "
            , Fixpoint.Code.inline "\\0"
            , text " guard."
            ]
        , p []
            [ text "Because the columns sit at known byte offsets, "
            , text "“bind the first "
            , Fixpoint.Code.inline "k"
            , text " columns to constants and enumerate the rest”"
            , text " becomes exactly a "
            , em [] [ text "byte-prefix" ]
            , text " lookup on the relation’s DAFSA. The DAFSA’s two strongest primitives — exact-key lookup and prefix enumeration — are precisely the two most common join access patterns."
            ]
        , h2 [ id "per-relation" ] [ text "One DAFSA per relation" ]
        , p []
            [ text "Each relation "
            , Fixpoint.Code.inline "R"
            , text " of arity "
            , Fixpoint.Code.inline "a_R"
            , text " has its own DAFSA, plus its own write-ahead log. Per-relation DAFSAs preserve prefix-enum selectivity, isolate compaction, and keep schema / arity / permutation-index metadata clean."
            ]
        , p []
            [ text "A separate "
            , strong [] [ text "symbol DAFSA" ]
            , text " maps interned strings to u32 symbol ids — a forward "
            , Fixpoint.Code.inline "str→sym"
            , text " DAFSA plus a reverse "
            , Fixpoint.Code.inline "sym→str"
            , text " array."
            ]
        , h2 [ id "compact" ] [ text "Why the DAFSA makes the store compact" ]
        , p []
            [ text "The DAFSA is a "
            , em [] [ text "minimized acyclic" ]
            , text " DFA: during construction, states with identical outgoing transition structures are "
            , strong [] [ text "merged" ]
            , text " into one shared state. Because every fact is a fixed-width key ending in a common "
            , Fixpoint.Code.inline "\\0"
            , text " guard, the tails of keys that share a suffix collapse into a single path. A relation of "
            , Fixpoint.Code.inline "N"
            , text " facts whose keys share long common suffixes stores those suffixes "
            , em [] [ text "once" ]
            , text ", not "
            , Fixpoint.Code.inline "N"
            , text " times."
            ]
        , p []
            [ text "Concretely: a store with thousands of "
            , Fixpoint.Code.inline "edge(a,b)"
            , text " tuples that all fan out over the same few destination columns keeps the shared tail states exactly once. The whole database is the sum of these per-relation DAFSAs (plus the interner), and the read path is "
            , strong [] [ text "mmap’d zero-copy" ]
            , text " — there is no secondary index to maintain and no deserialization; the DAFSA bytes on disk "
            , em [] [ text "are" ]
            , text " the queryable structure. This is what lets the engine serve reads as fast as a fact store can, from a footprint that is a fraction of a raw row store for suffix-heavy data."
            ]
        , p []
            [ text "The trade-off: a minimized DAG is not a B-tree. It gives exact-key lookup and prefix enumeration at O(key length), and (via the order-statistics subtree arrays) rank / select / range in time logarithmic in the number of distinct tuples — but it deliberately does not provide arbitrary-value random access. Order statistics and the sorted iterator close most of that gap; see "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/order-statistics/", attribute "data-mfe-route" "/datalog-dafsa/order-statistics" ] [ text "Order Statistics" ]
            , text "."
            ]
        , h2 [ id "lifecycle" ] [ text "Lifecycle: load → compile → publish → serve" ]
        , ol []
            [ li []
                [ strong [] [ text "Load" ]
                , text " facts into a database directory ("
                , Fixpoint.Code.inline "dl_load_facts"
                , text " bulk CSV, or incremental "
                , Fixpoint.Code.inline "dl_add_fact"
                , text " / "
                , Fixpoint.Code.inline "dl_delete_fact"
                , text ")."
                ]
            , li []
                [ strong [] [ text "Declare" ]
                , text " relations and "
                , strong [] [ text "compile" ]
                , text " rules ("
                , Fixpoint.Code.inline "dl_load_rules"
                , text " + "
                , Fixpoint.Code.inline "dl_compile"
                , text "), running the semi-naive fixpoint VM to materialize derived relations."
                ]
            , li []
                [ strong [] [ text "Publish" ]
                , text " a versioned snapshot atomically ("
                , Fixpoint.Code.inline "dl_publish_snapshot"
                , text ": save the interner + all relations, flip the "
                , Fixpoint.Code.inline "CURRENT"
                , text " pointer)."
                ]
            , li []
                [ strong [] [ text "Serve" ]
                , text " reads from mmap’d read-only views ("
                , Fixpoint.Code.inline "dl_query"
                , text " / "
                , Fixpoint.Code.inline "dl_query_bound"
                , text " / "
                , Fixpoint.Code.inline "dl_pattern"
                , text ") instead of running the VM."
                ]
            ]
        , p [] [ text "This is a single-writer / multiple-reader model: one writer, readers on mmap." ]
        , h2 [ id "durability" ] [ text "Durability" ]
        , p []
            [ text ""
            , Fixpoint.Code.inline "dl_add_fact"
            , text " / "
            , Fixpoint.Code.inline "dl_delete_fact"
            , text " append to a per-relation WAL and fsync before committing in memory. A "
            , Fixpoint.Code.inline "fcntl"
            , text " single-writer lock guards the database. The interner is saved durably, and it is ordered "
            , em [] [ text "before" ]
            , text " WAL records so crash recovery can decode symbol ids. WAL compaction triggers at 25% of the relation size."
            ]
        , h2 [ id "strategies" ] [ text "Join & evaluation strategies" ]
        , h3 [] [ text "Prefix enumeration = index-nested-loop join" ]
        , p []
            [ text "The VM implements joins as index-nested-loop: for each tuple, bind the shared leading columns to constants and prefix-enumerate the next relation (via "
            , Fixpoint.Code.inline "relation.c"
            , text "’s prefix walker, which DFS-traverses the DAFSA from the prefix state). Non-leading-column joins are handled by per-relation "
            , strong [] [ text "permutation indices" ]
            , text " — a permuted DAFSA for every column-prefix the compiler sees used as a join key — plus a hash-join fallback for the rest. "
            , Fixpoint.Code.inline "min"
            , text "/"
            , Fixpoint.Code.inline "max"
            , text " aggregates fall out of the big-endian encoding (extreme prefix = extreme key)."
            ]
        , h3 [] [ text "Semi-naive fixpoint + stratification" ]
        , p []
            [ text "Recursive rules are evaluated with a semi-naive fixpoint (only the delta is propagated each round). A stratification pass assigns strata and rejects unstratifiable programs (negation through recursion, or a strict cycle through "
            , Fixpoint.Code.inline "range"
            , text "). Derived relations in a recursive SCC are materialized before any same-stratum dependent reads them."
            ]
        , h3 [] [ text "Bushy joins" ]
        , p []
            [ text "Negation-free rules optionally take a binary-tree (bushy) join plan when a natural 2-partition with a low cut width exists (compiled-time toggle, default on). Otherwise a greedy left-deep join reorders the body atoms by ascending estimated cardinality."
            ]
        , h3 [] [ text "Permutation-index selection" ]
        , p []
            [ text "For a non-leading-column join, the compiler picks between a permuted DAFSA ("
            , Fixpoint.Code.inline "OP_LOOKUP_PERM"
            , text ") and a slot-free hash join ("
            , Fixpoint.Code.inline "OP_HASH_JOIN"
            , text ") using a cardinality cost gate. A recursive body atom is always served by a permutation index (never a hash join, which would read a stale DAFSA), and a hash-join fallback is used when no index is worth building. See "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/order-statistics/#perm", attribute "data-mfe-route" "/datalog-dafsa/order-statistics" ] [ text "Order Statistics" ]
            , text "."
            ]
        , h3 [] [ text "Magic-sets / QSQ top-down" ]
        , p []
            [ text "The "
            , Fixpoint.Code.inline "dl_query_magic"
            , text " family re-evaluates a "
            , em [] [ text "scoped" ]
            , text " fixpoint seeded by the bound goal arguments, materializing only the reachable IDB slice — the result is byte-identical to a bound query over the fully materialized relation. The top-down / QSQ path evaluates the same adorned + magic program but schedules it demand-driven (an SLG worklist over subqueries) instead of as a forward fixpoint. Both are opt-in per-query paths and are available through the C API only."
            ]
        , h3 [] [ text "Incremental view maintenance (IVM)" ]
        , p []
            [ text "After rules are compiled and a snapshot is published, subsequent fact insertions (and deletions) are maintained incrementally where eligible (delta propagation, DRed for deletions, aggregate maintenance, bulk) instead of re-running the full fixpoint. Programs using list builtins or variadics are excluded from the incremental paths and always evaluate via the full fixpoint — never silently mis-evaluated."
            ]
        , h3 [] [ text "Pull-iterator + merge-join" ]
        , p []
            [ text "A resumable pull-based sorted iterator ("
            , Fixpoint.Code.inline "dl_iter_*"
            , text ") exposes each relation in ascending key order, and "
            , Fixpoint.Code.inline "dl_merge_join"
            , text " equi-joins two sorted iterators on a shared leading prefix, streaming pairs in sorted order. See "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/order-statistics/#iter", attribute "data-mfe-route" "/datalog-dafsa/order-statistics" ] [ text "Order Statistics" ]
            , text "."
            ]
        , h3 [] [ text "Lazy ", Fixpoint.Code.inline "OP_RANGE" ]
        , p []
            [ text "The "
            , Fixpoint.Code.inline "range(X, Rel, Lo, Hi)"
            , text " generator is a "
            , em [] [ text "lazy" ]
            , text " resumable generator over the pull-iterator (not an eager materialization): it skips to the lower bound, deduplicates consecutive leading-column values, stops at the upper bound, and can be short-circuited by an early-stopping consumer. It reads the live relation (never a stale snapshot view). See "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/order-statistics/#range", attribute "data-mfe-route" "/datalog-dafsa/order-statistics" ] [ text "Order Statistics" ]
            , text "."
            ]
        , h2 [ id "performance" ] [ text "Performance" ]
        , p []
            [ text "The engine is built around the DAFSA’s exact-lookup and prefix-walk primitives, giving index-nested-loop joins whose cost tracks the size of the "
            , em [] [ text "bound" ]
            , text " prefix rather than a full scan, and order statistics (rank / select / range / count) that run in time logarithmic in the number of distinct tuples via the DAFSA’s subtree arrays. The exact-characterizing benchmark is "
            , Fixpoint.Code.inline "tests/bench.c"
            , text " ("
            , Fixpoint.Code.inline "make bench"
            , text "); this site deliberately does not quote specific numbers."
            ]
        ]


timeTravelView : Html Msg
timeTravelView =
    div []
        [ h1 [] [ text "Time Travel" ]
        , p []
            [ text "Time-travel is a "
            , strong [] [ text "first-class feature" ]
            , text ", not an add-on: because the engine is built around a "
            , em [] [ text "publish-then-serve" ]
            , text " lifecycle and keeps every published snapshot by default, the database is a "
            , strong [] [ text "versioned point-in-time history" ]
            , text ". Any past state can be queried as if it were current."
            ]
        , p [] [ text "That buys real capabilities on a batch-write / read-heavy workload:" ]
        , ul []
            [ li []
                [ strong [] [ text "Audit & as-of analysis" ]
                , text " — answer “what did this derived relation contain two publishes ago?” without a replay or a full rebuild."
                ]
            , li []
                [ strong [] [ text "Immutable history" ]
                , text " — every version is a frozen, reproducible view; later writes can never retroactively change it."
                ]
            , li []
                [ strong [] [ text "Diff / evolution" ]
                , text " — compare the same goal across versions to see exactly how the database evolved between publishes."
                ]
            , li []
                [ strong [] [ text "Zero-cost snapshotting" ]
                , text " — the snapshot "
                , em [] [ text "is" ]
                , text " the normal read path (mmap views); keeping history is just not deleting the old version directories."
                ]
            ]
        , h2 [ id "cli" ] [ text "From the CLI" ]
        , p []
            [ text "The "
            , Fixpoint.Code.inline "dl"
            , text " CLI exposes the timeline directly — record a point-in-time with "
            , Fixpoint.Code.inline "publish"
            , text ", list the history with "
            , Fixpoint.Code.inline "versions"
            , text ", and run queries "
            , em [] [ text "as-of" ]
            , text " a past snapshot with "
            , Fixpoint.Code.inline "--version N"
            , text ":"
            ]
        , Fixpoint.Code.block
            [ text "$ ./dl -d /tmp/db publish\n"
            , text "Snapshot published.\n"
            , text "\n"
            , text "$ ./dl -d /tmp/db versions\n"
            , text "1\n"
            , text "2\n"
            , text "3\n"
            , text "\n"
            , text "# Query as-of a past snapshot — full-text and semantic search both support --version.\n"
            , text "$ ./dl -d /tmp/db search 'gpu rental' --top 10 --version 2\n"
            , text "$ ./dl -d /tmp/db vsearch 'GPU rental' --k 10 --version 2"
            ]
        , p []
            [ text "The version-aware C API is "
            , Fixpoint.Code.inline "dl_query_version"
            , text ", "
            , Fixpoint.Code.inline "dl_search_version"
            , text ", and "
            , Fixpoint.Code.inline "dl_vector_search_version"
            , text " (see "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/api/", attribute "data-mfe-route" "/datalog-dafsa/api" ] [ text "the C API" ]
            , text ")."
            ]
        , h2 [ id "versions" ] [ text "Versioned snapshots" ]
        , p []
            [ text "Each successful publish produces a monotonically increasing version number starting at 1. Enumerate the available versions ascending with "
            , Fixpoint.Code.inline "dl_snapshot_versions"
            , text ", using the two-call idiom:"
            ]
        , Fixpoint.Code.block
            [ text "long total = dl_snapshot_versions(db, NULL, 0);   /* size */\n"
            , text "uint32_t *vers = malloc((size_t)total * sizeof(*vers));\n"
            , text "dl_snapshot_versions(db, vers, (size_t)total);    /* fill */\n"
            , text "free(vers);"
            ]
        , p []
            [ text "It returns the "
            , em [] [ text "total" ]
            , text " number of versions even when the output buffer is smaller (filling at most "
            , Fixpoint.Code.inline "cap"
            , text " entries), returns 0 when no snapshot has been published, and -1 on a NULL "
            , Fixpoint.Code.inline "db"
            , text "."
            ]
        , h2 [ id "asof" ] [ text "As-of queries" ]
        , p []
            [ text "Query a relation as of a specific published version with "
            , Fixpoint.Code.inline "dl_query_version"
            , text ", or bind leading columns with "
            , Fixpoint.Code.inline "dl_query_bound_version"
            , text ":"
            ]
        , Fixpoint.Code.block
            [ text "long n = dl_query_version(db, version, \"edge\", cb, user);\n"
            , text "long m = dl_query_bound_version(db, version, \"edge\", leading, k, cb, user);"
            ]
        , p [] [ text "Semantics and guarantees:" ]
        , ul []
            [ li []
                [ text "As-of reads use an explicit version and read that version’s manifest + mmap view from disk; "
                , Fixpoint.Code.inline "db->snap_version"
                , text " is never mutated, so live/current routing is untouched."
                ]
            , li []
                [ text "As-of views are "
                , strong [] [ text "immutable" ]
                , text ": add/delete operations after a publish never change an earlier version’s view. The live "
                , Fixpoint.Code.inline "dl_query"
                , text " stays on the current version until the next publish."
                ]
            , li []
                [ text "A nonexistent version (including 0), a NULL goal/relation absent from that version, or a NULL callback is a "
                , strong [] [ text "loud" ]
                , text " -1 — never a silently-empty result."
                ]
            , li [] [ text "Variadic relations are supported (arity-mixed tuples are returned)." ]
            ]
        , h2 [ id "retain" ] [ text "Retention" ]
        , p []
            [ text "By default every version is kept forever. To bound disk usage, opt in to prune-to-N with "
            , Fixpoint.Code.inline "dl_set_snapshot_retain"
            , text ": after each successful publish, the oldest versions beyond the most-recent "
            , Fixpoint.Code.inline "n"
            , text " are pruned. "
            , Fixpoint.Code.inline "n == 0"
            , text " (the default) restores keep-all."
            ]
        , Fixpoint.Code.block
            [ text "dl_set_snapshot_retain(db, 5);   /* keep the 5 most-recent versions */\n"
            , text "dl_set_snapshot_retain(db, 0);   /* back to keep-all */"
            ]
        , p []
            [ text "A pruned version is gone — querying it returns -1 (loud), matching the nonexistent-version contract."
            ]
        , h2 [ id "model" ] [ text "Concurrency model" ]
        , p []
            [ text "This fits the engine’s single-writer / multiple-reader model. Readers hold mmap views and keep reading valid data even after a retention prune unlinks the underlying snapshot directory. A reader holding an mmap of "
            , Fixpoint.Code.inline "snapshots/<V>/<rel>.dafsa"
            , text " keeps reading valid data after the pruner removes the directory — the unlink does not disturb the open mapping."
            ]
        ]


vectorSearchView : Html Msg
vectorSearchView =
    div []
        [ h1 [] [ text "Semantic ", Fixpoint.Hero.fx [ text "vector search" ] ]
        , p []
            [ text "Meaning-aware retrieval on top of the full-text index. The engine embeds text with a real bge-small model (via the "
            , Fixpoint.Code.inline "dl-embed"
            , text " C++ tool), retrieves candidates with "
            , strong [] [ text "multi-index hashing (MIH) over ITQ bit-codes" ]
            , text ", and re-ranks by exact int8 cosine — all stored in-relation and "
            , strong [] [ text "snapshot-versioned" ]
            , text " like everything else."
            ]
        , h2 [] [ text "Quickstart" ]
        , p []
            [ text "The vector tier is opt-in and adds a vendored "
            , a [ href "https://github.com/ggml-org/ggml" ] [ text "ggml" ]
            , text " submodule (v0.20.2) plus the model:"
            ]
        , Fixpoint.Code.block
            [ Fixpoint.Code.c "# One-time: materialize ggml + the bge-small model (git-lfs tracked)."
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " "
            , Fixpoint.Code.g "git submodule update --init vendor/ggml"
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " "
            , Fixpoint.Code.g "git lfs pull"
            , text "             "
            , Fixpoint.Code.c "# models/bge-small-en-v1.5-f16.gguf (67 MB)"
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " "
            , Fixpoint.Code.g "make dl-embed"
            , text "           "
            , Fixpoint.Code.c "# build ./dl-embed (needs cmake)"
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " "
            , Fixpoint.Code.g "./dl-embed self-test"
            , text "   "
            , Fixpoint.Code.c "# golden-embedding gate (cosine >= 0.9999 vs reference)"
            , text "\n\n"
            , Fixpoint.Code.c "# Embed the corpus into the vector index (18 relations + one publish)."
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " "
            , Fixpoint.Code.g "./dl-embed pipeline --db /tmp/db"
            , text "\n\n"
            , Fixpoint.Code.c "# Query it."
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " "
            , Fixpoint.Code.g "./dl -d /tmp/db vsearch 'affordable GPU rental' --k 10"
            , text "\n"
            , Fixpoint.Code.k "$"
            , text " "
            , Fixpoint.Code.g "./dl -d /tmp/db vhybrid 'gpu rental' 'affordable GPU rental' --k 10"
            ]
        , h2 [] [ text "How it works" ]
        , p []
            [ text "Each entity is embedded and "
            , strong [] [ text "binarized to a 256-bit ITQ signature" ]
            , text ". The signature is split into "
            , Fixpoint.Code.inline "m=16"
            , text " "
            , strong [] [ text "bands" ]
            , text "; for band "
            , Fixpoint.Code.inline "j"
            , text " the store keeps a fixed-arity relation "
            , Fixpoint.Code.inline "__sig{j}__(band_u32, entity_sym_id)"
            , text " — a postings index from bit-substring to entity sym-id, the same shape as the full-text postings."
            ]
        , Fixpoint.Code.block
            [ text "__sig0__..__sig15__   (arity 2)  band_value -> entity_sym_id    MIH candidate postings\n"
            , text "__vec_q__            (arity 3)  entity_sym_id, chunk_idx, packed_4x_int8_u32   re-rank vectors\n"
            , text "__itq_basis__        (arity 3)  dim_i, dim_j, float32_bits_u32   ITQ encode matrix (pinned)"
            ]
        , p []
            [ text "A query embeds the text with the same model, ITQ-encodes it, then for each band enumerates the query’s substring variants within the pigeonhole budget "
            , Fixpoint.Code.inline "⌊r/m⌋"
            , text ", probes each via "
            , Fixpoint.Code.inline "dl_prefix"
            , text " over "
            , Fixpoint.Code.inline "__sig{j}__"
            , text ", unions the candidates across bands, "
            , strong [] [ text "filters to live entities" ]
            , text ", and re-ranks by exact integer int8 cosine. All reads are snapshot-versioned like any other relation, so "
            , Fixpoint.Code.inline "dl_vector_search_version"
            , text " and "
            , Fixpoint.Code.inline "dl search --version"
            , text " give you "
            , strong [] [ text "time-travelling semantic search" ]
            , text " for free."
            ]
        , h2 [] [ text "The CLI" ]
        , h3 [] [ text "vsearch" ]
        , p [] [ text "Semantic vector search over the MIH postings + int8 re-rank." ]
        , Fixpoint.Code.block
            [ text "dl [-d <dir>] vsearch '<query>' [--k N] [--radius R] [--version V]\n"
            , text "                                  [--sig <hex64>] [--ivec <hex768>]"
            ]
        , p []
            [ text ""
            , Fixpoint.Code.inline "--version V"
            , text " queries as-of a published snapshot (0 = live). For a programmatic path with no model, pass the pre-encoded query as "
            , Fixpoint.Code.inline "--sig"
            , text " (8 u32 = 64 hex) and "
            , Fixpoint.Code.inline "--ivec"
            , text " (96 u32 = 768 hex)."
            ]
        , h3 [] [ text "vhybrid" ]
        , p [] [ text "Lexical ∩ semantic hybrid: intersect ", Fixpoint.Code.inline "search", text " results with ", Fixpoint.Code.inline "vsearch", text " candidates, then re-rank the intersection." ]
        , Fixpoint.Code.block
            [ text "dl [-d <dir>] vhybrid '<terms>' '<query>' [--k N] [--radius R] [--version V]"
            ]
        , h3 [] [ text "search (full-text)" ]
        , p []
            [ text "The symbolic half: AND-intersect tokenized terms over "
            , Fixpoint.Code.inline "__postings__"
            , text " and rank by co-occurrence. "
            , Fixpoint.Code.inline "--version N"
            , text " queries as-of a snapshot."
            ]
        , Fixpoint.Code.block
            [ text "dl [-d <dir>] search '<terms>' [--top N] [--version N]"
            ]
        , h2 [] [ text "dl-embed" ]
        , p []
            [ text "The C++ embedding tool (ggml-based). It runs the real "
            , strong [] [ text "bge-small-en-v1.5" ]
            , text " model in-process — the weights are the actual pretrained model (git-lfs tracked under "
            , Fixpoint.Code.inline "models/"
            , text "), not a re-implementation. The C++ provides the inference runtime: GGUF load + BERT forward pass + CLS pooling + L2 normalization, a WordPiece tokenizer that auto-detects the llama.cpp WPM vocab convention, and the ITQ fit/encode + int8 quantization."
            ]
        , Fixpoint.Code.block
            [ text "dl-embed pipeline --db DIR     embed corpus + emit vector relations + publish\n"
            , text "dl-embed encode --db DIR QRY   print 'sig_hex ivec_hex' for a query (CLI consumes this)\n"
            , text "dl-embed embed QRY             print the raw 384-float embedding\n"
            , text "dl-embed tokenize QRY          print token ids + strings (debug)\n"
            , text "dl-embed self-test             math/tokenizer checks (+ golden-embedding gate if model present)\n"
            , text "dl-embed dump-tensors [PATH]   list GGUF tensors\n"
            , text "dl-embed fetch-model           download the bge-small GGUF to models/"
            ]
        , p []
            [ text "The "
            , Fixpoint.Code.inline "dl vsearch"
            , text "/"
            , Fixpoint.Code.inline "vhybrid"
            , text " commands "
            , em [] [ text "fork/execve" ]
            , text " "
            , Fixpoint.Code.inline "dl-embed encode"
            , text " to embed the query (no shell, no Python). The golden gate ("
            , Fixpoint.Code.inline "./dl-embed self-test"
            , text ") embeds reference strings and asserts cosine ≥ 0.9999 against the reference model — the numeric proof that the C++ forward pass is correct."
            ]
        , h2 [] [ text "Design" ]
        , p []
            [ text "The full design of record lives in the design docs: "
            , a [ href "https://github.com/fixpoint-linux/datalog-dafsa/blob/main/docs/datalog-dafsa-vector-search.md" ] [ text "datalog-dafsa-vector-search.md" ]
            , text " (MIH over ITQ, the integration seam, honest ceiling) and the int8-in-store note. In short: in-store MIH is competitive with HNSW at "
            , strong [] [ text "~1e5–1e6 entities" ]
            , text " and wins the consolidation story — one identity space, one snapshot, one WAL, one crash-recovery story."
            ]
        ]


orderStatisticsView : Html Msg
orderStatisticsView =
    div []
        [ h1 [] [ text "Order Statistics" ]
        , p []
            [ text "Because every fact is stored as a fixed-width u32 "
            , em [] [ text "big-endian" ]
            , text " key, the numeric order of keys is exactly their "
            , em [] [ text "lexicographic" ]
            , text " order. That single fact makes positional queries — rank, select, range count, and an ordered enumeration — natural and cheap over a DAFSA’s sorted structure. Order statistics are available through the C API only; there is no Datalog syntax and no CLI command for them."
            ]
        , h2 [ id "core" ] [ text "rank / select / range_count / count" ]
        , p [] [ text "The core four functions operate on the ", em [] [ text "leading" ], text " column order (the natural key order):" ]
        , Fixpoint.Code.block
            [ text "uint64_t dl_rank(db, \"r\", cols, arity);          /* # tuples strictly < cols */\n"
            , text "int      dl_select(db, \"r\", k, cols_out, arity); /* k-th tuple (0-indexed, lex) */\n"
            , text "uint64_t dl_range_count(db, \"r\", lo, hi, arity); /* # tuples in [lo, hi) */\n"
            , text "uint64_t dl_count(db, \"r\");                      /* O(1) distinct-tuple count */"
            ]
        , ul []
            [ li []
                [ Fixpoint.Code.inline "dl_rank"
                , text " counts distinct tuples strictly lexicographically smaller than "
                , Fixpoint.Code.inline "cols"
                , text ". An absent key ranks at its insertion position (between neighbours, below the minimum, or at the count)."
                ]
            , li []
                [ Fixpoint.Code.inline "dl_select"
                , text " writes the k-th tuple (0-indexed, lex order); returns -1 if k is out of range."
                ]
            , li []
                [ Fixpoint.Code.inline "dl_range_count"
                , text " returns "
                , Fixpoint.Code.inline "rank(hi) − rank(lo)"
                , text " for the half-open range "
                , Fixpoint.Code.inline "[lo, hi)"
                , text "."
                ]
            , li []
                [ Fixpoint.Code.inline "dl_count"
                , text " is an O(1) distinct-tuple count backed by a memoized subtree array."
                ]
            ]
        , p []
            [ text "When a snapshot has been published ("
            , Fixpoint.Code.inline "db->snap_version > 0"
            , text "), these read the mmap snapshot view, so they reflect the "
            , em [] [ text "published" ]
            , text " state even if the live in-memory relation has since been mutated; otherwise they read the in-memory relation."
            ]
        , h2 [ id "bound" ] [ text "Bound variants" ]
        , p []
            [ text "The "
            , Fixpoint.Code.inline "_bound"
            , text " family restricts to tuples whose first "
            , Fixpoint.Code.inline "k"
            , text " columns equal a "
            , Fixpoint.Code.inline "leading"
            , text " bound, then applies rank / select / range_count to the suffix (the remaining "
            , Fixpoint.Code.inline "arity-k"
            , text " columns). The "
            , Fixpoint.Code.inline "cols"
            , text "/"
            , Fixpoint.Code.inline "lo"
            , text "/"
            , Fixpoint.Code.inline "hi"
            , text " are full arity-length tuples whose first "
            , Fixpoint.Code.inline "k"
            , text " entries must equal "
            , Fixpoint.Code.inline "leading"
            , text ". "
            , Fixpoint.Code.inline "k==0"
            , text " means no bound (degenerates to the base family)."
            ]
        , Fixpoint.Code.block
            [ text "uint64_t dl_rank_bound(db, \"r\", leading, k, cols, arity);\n"
            , text "int      dl_select_bound(db, \"r\", leading, k, idx, cols_out, arity);\n"
            , text "uint64_t dl_range_count_bound(db, \"r\", leading, k, lo, hi, arity);"
            ]
        , h2 [ id "perm" ] [ text "Permuted order statistics" ]
        , p []
            [ text "The "
            , Fixpoint.Code.inline "_perm"
            , text " family evaluates order statistics over a "
            , em [] [ text "permuted" ]
            , text " view — an order-by on a "
            , em [] [ text "non-leading" ]
            , text " column. A permutation index is declared with "
            , Fixpoint.Code.inline "dl_db_declare_perm"
            , text " (an exported helper in the internal "
            , Fixpoint.Code.inline "permindex.h"
            , text ", not part of the public "
            , Fixpoint.Code.inline "dl.h"
            , text " surface); then "
            , Fixpoint.Code.inline "perm[j]"
            , text " is the original column that appears at permuted position "
            , Fixpoint.Code.inline "j"
            , text ". For example "
            , Fixpoint.Code.inline "perm = {1, 0}"
            , text " over an arity-2 relation orders tuples by column 1, then column 0 as the tiebreaker."
            ]
        , p []
            [ text ""
            , Fixpoint.Code.inline "cols"
            , text "/"
            , Fixpoint.Code.inline "lo"
            , text "/"
            , Fixpoint.Code.inline "hi"
            , text " are full tuples in the "
            , em [] [ text "original" ]
            , text " column order. Rank and range-count forward-map the input to permuted order before ranking; select inverse-maps its permuted-order result back to original order, so a select→rank round-trip over the same perm is the identity. The permuted relation is built on demand and rebuilt if dirty — a stale index is never silently used."
            ]
        , Fixpoint.Code.block
            [ text "uint64_t dl_rank_perm(db, rel, perm_id, cols, arity);\n"
            , text "int      dl_select_perm(db, rel, perm_id, k, cols_out, arity);\n"
            , text "uint64_t dl_range_count_perm(db, rel, perm_id, lo, hi, arity);\n"
            , text "int      dl_db_perm_count(db);   /* number of declared perm indices */"
            ]
        , p []
            [ text "The compiler also "
            , em [] [ text "selects" ]
            , text " permutation indices automatically for non-leading-column joins ("
            , a [ href "https://fixpointlinux.org/datalog-dafsa/architecture/#strategies", attribute "data-mfe-route" "/datalog-dafsa/architecture" ] [ text "see Architecture" ]
            , text ")."
            ]
        , h2 [ id "iter" ] [ text "Pull-iterator + merge-join" ]
        , p []
            [ text ""
            , Fixpoint.Code.inline "dl_iter_*"
            , text " is a resumable pull-based cursor over a relation in ascending key order. It reads from the mmap snapshot view when a snapshot is current, else the in-memory relation. Because a DAFSA is a minimal acyclic DFA with per-state transitions sorted by symbol, pre-order DFS == byte/lex == u32BE numeric order; from a bound state every final state sits at the same fixed depth, so emit-then-backtrack yields exactly one tuple per "
            , Fixpoint.Code.inline "dl_iter_next"
            , text "."
            ]
        , Fixpoint.Code.block
            [ text "dl_iter *it = dl_iter_open(db, \"r\", leading, k);   /* k==0: all tuples */\n"
            , text "uint32_t row[MAXA];\n"
            , text "while (dl_iter_next(it, row) == 1) { /* consume ascending tuple */ }\n"
            , text "dl_iter_close(it);"
            ]
        , p []
            [ text ""
            , Fixpoint.Code.inline "dl_merge_join"
            , text " equi-joins two sorted iterators on their first "
            , Fixpoint.Code.inline "jcols"
            , text " columns, streaming matching pairs in sorted order with cross-product semantics (duplicates preserved). Both iterators are left exhausted on return; a non-zero callback return stops the join early."
            ]
        , Fixpoint.Code.block
            [ text "long n = dl_merge_join(left, right, jcols, join_cb, user);"
            ]
        , h2 [ id "range" ] [ text "Lazy ", Fixpoint.Code.inline "OP_RANGE" ]
        , p []
            [ text "The Datalog "
            , Fixpoint.Code.inline "range(X, Rel, Lo, Hi)"
            , text " builtin (see the "
            , a [ href "https://fixpointlinux.org/datalog-dafsa/language/#range", attribute "data-mfe-route" "/datalog-dafsa/language" ] [ text "Language Reference" ]
            , text ") is backed by a "
            , em [] [ text "lazy" ]
            , text " resumable generator over the pull-iterator. It:"
            ]
        , ul []
            [ li [] [ text "opens a k=0 cursor and ", strong [] [ text "skips" ], text " leading-column values below the lower bound;" ]
            , li [] [ text "", strong [] [ text "deduplicates" ], text " consecutive equal leading-column values (yielding distinct col0 values, not every tuple);" ]
            , li [] [ text "", strong [] [ text "stops" ], text " at the upper bound;" ]
            , li [] [ text "can be ", strong [] [ text "short-circuited" ], text " by an early-stopping consumer." ]
            ]
        , p []
            [ text "It reads the live relation (never a stale snapshot view), and range over a recursive relation is rejected at compile time."
            ]
        ]


typedProjectsView : Html Msg
typedProjectsView =
    div []
        [ h1 [] [ text "Typed Projects (", Fixpoint.Code.inline "dlp", text ")" ]
        , p []
            [ Fixpoint.Code.inline "dlp"
            , text " (“dl-project”) is a "
            , strong [] [ text "typed, project-based workflow" ]
            , text " layered on top of the engine. Instead of loading untyped facts and rules directly, you define a database "
            , em [] [ text "schema" ]
            , text " in Dhall, load data validated against it, and write Datalog rules whose relations are "
            , strong [] [ text "typechecked against the schema before they compile" ]
            , text ". This turns silent type errors into clear, early diagnostics."
            ]
        , Fixpoint.Callout.note
            [ Fixpoint.Code.inline "dlp"
            , text " is a separate binary from the low-level "
            , Fixpoint.Code.inline "dl"
            , text " CLI. It links the engine together with the "
            , a [ href "https://github.com/jmars/dhall-c" ] [ text "dhall-c" ]
            , text " interpreter in-process, so the schema is typechecked and normalized without any on-disk intermediate. The gcc core build is untouched — "
            , Fixpoint.Code.inline "dlp"
            , text " is opt-in via "
            , Fixpoint.Code.inline "make dlp DHALLC=<path-to-dhall-c>"
            , text "."
            ]
        , h2 [ id "why" ] [ text "Why typed schemas?" ]
        , p []
            [ text "Every engine value is a "
            , Fixpoint.Code.inline "u32"
            , text " — either a raw integer or an interned symbol id. Without column types, an int column and a symbol column are indistinguishable, and a rule can silently mix them (a real correctness trap). The typed workflow closes that gap:"
            ]
        , ul []
            [ li []
                [ strong [] [ text "Closed world" ]
                , text " — every relation is declared in "
                , Fixpoint.Code.inline "schema.dhall"
                , text "; using an undeclared relation is an error that points at the schema."
                ]
            , li []
                [ strong [] [ text "Per-column types" ]
                , text " — eight built-in types spanning flat scalars (Natural, Text, Bool, Char, Date, Timestamp, Signed) and parameterized (List, Optional, Enum), checked on both data and rules."
                ]
            , li []
                [ strong [] [ text "Rule typechecking" ]
                , text " — every rule is checked for "
                , em [] [ text "occurrence consistency" ]
                , text " (a variable can’t be Natural in one atom and Text in another), with "
                , Fixpoint.Code.inline "file:line:col"
                , text " diagnostics."
                ]
            , li []
                [ strong [] [ text "Typed data loading" ]
                , text " — CSV/JSON rows are validated and coerced per column before they enter the store."
                ]
            ]
        , p []
            [ text "For example, the rule "
            , Fixpoint.Code.inline "tc(A, W) :- weight(A, W)."
            , text " is rejected: "
            , Fixpoint.Code.inline "W"
            , text " is "
            , Fixpoint.Code.inline "Natural"
            , text " in "
            , Fixpoint.Code.inline "weight.w"
            , text " but "
            , Fixpoint.Code.inline "Text"
            , text " in "
            , Fixpoint.Code.inline "tc.dst"
            , text " — a bug the untyped engine would silently mis-evaluate."
            ]
        , h2 [ id "layout" ] [ text "Project layout" ]
        , p [] [ text "A database project is a directory with a few conventional subdirectories:" ]
        , Fixpoint.Code.block
            [ text "mydb/\n"
            , text "  schema.dhall      # the typed schema (the contract)\n"
            , text "  data/             # EDB CSV/JSON inputs, file stem = relation name\n"
            , text "  rules/            # .datalog rule files (concatenated, sorted)\n"
            , text "  .build/           # dlp-owned: build snapshot, schema.json echo"
            ]
        , h2 [ id "schema" ] [ text "The schema DSL" ]
        , p []
            [ text "The schema is "
            , strong [] [ text "Dhall-as-data" ]
            , text ": self-contained "
            , Fixpoint.Code.inline "let"
            , text "s with a final "
            , Fixpoint.Code.inline ": Schema"
            , text " annotation. Each relation declares its name, arity, and per-column type. The column-type union spans "
            , strong [] [ text "flat scalars" ]
            , text " (all raw u32: "
            , Fixpoint.Code.inline "Natural"
            , text ", "
            , Fixpoint.Code.inline "Text"
            , text " / interned symbol, "
            , Fixpoint.Code.inline "Bool"
            , text ", "
            , Fixpoint.Code.inline "Char"
            , text ", "
            , Fixpoint.Code.inline "Date"
            , text " =yyyymmdd, "
            , Fixpoint.Code.inline "Timestamp"
            , text " =epoch seconds, "
            , Fixpoint.Code.inline "Signed"
            , text " =i32 zigzag) and "
            , strong [] [ text "parameterized" ]
            , text " types ("
            , Fixpoint.Code.inline "List"
            , text " / "
            , Fixpoint.Code.inline "Optional"
            , text " of a flat element type, and "
            , Fixpoint.Code.inline "Enum"
            , text " with a fixed value set). The column-type union uses an "
            , em [] [ text "empty-record payload" ]
            , text " to tag a flat type, and a payload record to carry a parameterized type’s element type / value set:"
            ]
        , Fixpoint.Code.block
            [ text "let Elem = < Natural : {=} | Text : {=} | Bool : {=} | Char : {=} |\n"
            , text "             Date : {=} | Timestamp : {=} | Signed : {=} >\n"
            , text "let ColumnType = < Natural : {=} | Text : {=} | Bool : {=} | Char : {=} |\n"
            , text "                   Date : {=} | Timestamp : {=} | Signed : {=} |\n"
            , text "                   List : { elem : Elem } | Optional : { elem : Elem } |\n"
            , text "                   Enum : { values : List Text } >\n"
            , text "let Column = { name : Text, type : ColumnType }\n"
            , text "let Relation = { name : Text, columns : List Column }\n"
            , text "let Schema = { relations : List Relation }\n"
            , text "in { relations =\n"
            , text "     [ { name = \"node\", columns = [ { name = \"id\", type = < Text = {=} > },\n"
            , text "                                    { name = \"tags\", type = < List = { elem = < Text = {=} > } > },\n"
            , text "                                    { name = \"nick\", type = < Optional = { elem = < Text = {=} > } > },\n"
            , text "                                    { name = \"color\", type = < Enum = { values = [ \"red\", \"green\" ] } > } ] } ]\n"
            , text "   } : Schema"
            ]
        , p []
            [ text "Coercion is per-type on load: "
            , Fixpoint.Code.inline "Text"
            , text "/"
            , Fixpoint.Code.inline "Enum"
            , text " interned, "
            , Fixpoint.Code.inline "Natural"
            , text " "
            , Fixpoint.Code.inline "^[0-9]+$"
            , text " ≤ 4294967295, "
            , Fixpoint.Code.inline "Bool"
            , text " "
            , Fixpoint.Code.inline "true/false/0/1"
            , text " (CSV) or a JSON boolean, "
            , Fixpoint.Code.inline "Char"
            , text " one Unicode codepoint, "
            , Fixpoint.Code.inline "Date"
            , text " "
            , Fixpoint.Code.inline "yyyy-mm-dd"
            , text ", "
            , Fixpoint.Code.inline "Timestamp"
            , text " unix-seconds integer, "
            , Fixpoint.Code.inline "Signed"
            , text " signed i32, "
            , Fixpoint.Code.inline "List"
            , text " a JSON array or a bracketed quoted CSV cell "
            , Fixpoint.Code.inline "[a,b,c]"
            , text ", "
            , Fixpoint.Code.inline "Optional"
            , text " JSON "
            , Fixpoint.Code.inline "null"
            , text " or an empty CSV cell."
            ]
        , p []
            [ text "Arity is the length of "
            , Fixpoint.Code.inline "columns"
            , text " (1–8, enforced by the tool). Relations that appear as rule "
            , em [] [ text "heads" ]
            , text " are IDB (derived); loading data into them is an error."
            ]
        , h2 [ id "commands" ] [ text "Commands" ]
        , table []
            [ tr [] [ th [] [ text "Command" ], th [] [ text "Does" ] ]
            , tr [] [ td [] [ Fixpoint.Code.inline "dlp init [dir]" ], td [] [ text "Scaffold a project directory with an example schema." ] ]
            , tr [] [ td [] [ Fixpoint.Code.inline "dlp schema [dir]" ], td [] [ text "Dhall-typecheck + normalize ", Fixpoint.Code.inline "schema.dhall", text " and print the typed relations." ] ]
            , tr [] [ td [] [ Fixpoint.Code.inline "dlp check [dir]" ], td [] [ text "Validate schema + typecheck rules + dry-run data. ", strong [] [ text "No writes" ], text " — the CI command." ] ]
            , tr [] [ td [] [ Fixpoint.Code.inline "dlp build [dir]" ], td [] [ text "Check, then build a snapshot under ", Fixpoint.Code.inline ".build/", text " (declare EDB, load data, compile, publish)." ] ]
            , tr [] [ td [] [ Fixpoint.Code.inline "dlp query [dir] 'goal'" ], td [] [ text "Build in-process and evaluate a goal, e.g. ", Fixpoint.Code.inline "'tc(alice, X)'", text "." ] ]
            ]
        , p []
            [ text "Data files are matched to schema columns "
            , strong [] [ text "by name" ]
            , text " ("
            , em [] [ text "any order" ]
            , text "): CSV headers map to columns; JSON is an array of objects. CSV is text-typed ("
            , Fixpoint.Code.inline "Text"
            , text " takes any cell verbatim; "
            , Fixpoint.Code.inline "Natural"
            , text " requires "
            , Fixpoint.Code.inline "^[0-9]+$"
            , text " ≤ 4294967295). JSON is strict (number → "
            , Fixpoint.Code.inline "Natural"
            , text ", string → "
            , Fixpoint.Code.inline "Text"
            , text ")."
            ]
        , h2 [ id "errors" ] [ text "Catching the bug" ]
        , p []
            [ text "A mixed-type rule fails "
            , Fixpoint.Code.inline "dlp check"
            , text " and "
            , Fixpoint.Code.inline "dlp build"
            , text " with a precise diagnostic (here "
            , Fixpoint.Code.inline "<input>"
            , text " is the rule file):"
            ]
        , Fixpoint.Code.block
            [ text "$ dlp check .\n"
            , text "rules/reach.datalog: <input>:1:19: variable W is Natural here (weight) but Text at <input>:1:6 (tc)"
            ]
        , p []
            [ text "Data validation reports the exact row and column, e.g. "
            , Fixpoint.Code.inline "weight.csv:3:2: column 'w' expects Natural, got \"heavy\""
            , text "."
            ]
        , h2 [ id "build" ] [ text "Building dlp" ]
        , p []
            [ Fixpoint.Code.inline "dlp"
            , text " is an opt-in cosmocc build (the default gcc "
            , Fixpoint.Code.inline "make"
            , text " / "
            , Fixpoint.Code.inline "make test"
            , text " never touch it or dhall-c). With dhall-c as a sibling repo:"
            ]
        , Fixpoint.Code.block
            [ text "make dlp DHALLC=../dhall-c        # builds dlp/dlp\n"
            , text "make dlp-golden                   # end-to-end golden test (check/build/query)"
            ]
        ]